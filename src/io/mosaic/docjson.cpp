#include "io/mosaic/docjson.hpp"

#include "core/texture/texture_render.hpp"  // generatorTraits: the docio token mirror (S55-g)
#include "core/vector/boolean.hpp"          // bakedBooleanPath: the compound's old-build fallback

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <utility>

// The wire spellings of the document model (see docjson.hpp). Everything here is a pair of
// exact inverses, and the parsing side is STRICT: nullopt on any unknown token, wrong type, or
// out-of-contract value. Tolerance decisions (what to do when a payload is rejected) belong to
// docio.cpp, which owns the honesty counters.
namespace mosaic::io::native::detail {
namespace {

using nlohmann::json;

// ---- strict field access ---------------------------------------------------------------------

bool getD(const json& j, const char* k, double& out) {
    const auto it = j.find(k);
    if (it == j.end() || !it->is_number())
        return false;
    out = it->get<double>();
    return std::isfinite(out);
}

bool getF(const json& j, const char* k, float& out) {
    double d = 0.0;
    if (!getD(j, k, d))
        return false;
    out = static_cast<float>(d);
    return true;
}

bool getB(const json& j, const char* k, bool& out) {
    const auto it = j.find(k);
    if (it == j.end() || !it->is_boolean())
        return false;
    out = it->get<bool>();
    return true;
}

bool getU64(const json& j, const char* k, std::uint64_t& out) {
    const auto it = j.find(k);
    if (it == j.end() || !it->is_number_unsigned())
        return false;
    out = it->get<std::uint64_t>();
    return true;
}

bool getI(const json& j, const char* k, int& out) {
    const auto it = j.find(k);
    if (it == j.end() || !it->is_number_integer())
        return false;
    out = it->get<int>();
    return true;
}

bool getStr(const json& j, const char* k, std::string& out) {
    const auto it = j.find(k);
    if (it == j.end() || !it->is_string())
        return false;
    out = it->get<std::string>();
    return true;
}

const json* getArr(const json& j, const char* k) {
    const auto it = j.find(k);
    return (it != j.end() && it->is_array()) ? &*it : nullptr;
}

const json* getObj(const json& j, const char* k) {
    const auto it = j.find(k);
    return (it != j.end() && it->is_object()) ? &*it : nullptr;
}

// ---- enum token tables -----------------------------------------------------------------------

template <typename E, std::size_t N>
const char* tokenOf(const std::array<std::pair<E, const char*>, N>& table, E v) {
    for (const auto& [e, s] : table)
        if (e == v)
            return s;
    return table[0].second; // unreachable for in-range enums; a safe spelling regardless
}

template <typename E, std::size_t N>
std::optional<E> enumOf(const std::array<std::pair<E, const char*>, N>& table,
                        const std::string& s) {
    for (const auto& [e, t] : table)
        if (s == t)
            return e;
    return std::nullopt;
}

template <typename E, std::size_t N>
std::optional<E> enumField(const std::array<std::pair<E, const char*>, N>& table, const json& j,
                           const char* k) {
    std::string s;
    if (!getStr(j, k, s))
        return std::nullopt;
    return enumOf(table, s);
}

using core::BlendMode;
constexpr std::array<std::pair<BlendMode, const char*>, 23> kBlendTokens{{
    {BlendMode::Normal, "normal"},
    {BlendMode::Darken, "darken"},
    {BlendMode::Multiply, "multiply"},
    {BlendMode::ColorBurn, "color_burn"},
    {BlendMode::LinearBurn, "linear_burn"},
    {BlendMode::Lighten, "lighten"},
    {BlendMode::Screen, "screen"},
    {BlendMode::ColorDodge, "color_dodge"},
    {BlendMode::LinearDodge, "linear_dodge"},
    {BlendMode::Overlay, "overlay"},
    {BlendMode::SoftLight, "soft_light"},
    {BlendMode::HardLight, "hard_light"},
    {BlendMode::VividLight, "vivid_light"},
    {BlendMode::LinearLight, "linear_light"},
    {BlendMode::PinLight, "pin_light"},
    {BlendMode::Difference, "difference"},
    {BlendMode::Exclusion, "exclusion"},
    {BlendMode::Subtract, "subtract"},
    {BlendMode::Divide, "divide"},
    {BlendMode::Hue, "hue"},
    {BlendMode::Saturation, "saturation"},
    {BlendMode::Color, "color"},
    {BlendMode::Luminosity, "luminosity"},
}};

using core::AdjustmentKind;
constexpr std::array<std::pair<AdjustmentKind, const char*>, 35> kAdjustmentTokens{{
    {AdjustmentKind::BrightnessContrast, "brightness_contrast"},
    {AdjustmentKind::Levels, "levels"},
    {AdjustmentKind::Curves, "curves"},
    {AdjustmentKind::Exposure, "exposure"},
    {AdjustmentKind::HueSaturation, "hue_saturation"},
    {AdjustmentKind::ColorBalance, "color_balance"},
    {AdjustmentKind::Grayscale, "grayscale"},
    {AdjustmentKind::Invert, "invert"},
    {AdjustmentKind::Threshold, "threshold"},
    {AdjustmentKind::Posterize, "posterize"},
    {AdjustmentKind::PhotometricMatch, "photometric_match"},  // S55 estimate-from-layer grade
    {AdjustmentKind::GaussianBlur, "gaussian_blur"},          // S33 blur family
    {AdjustmentKind::BoxBlur, "box_blur"},
    {AdjustmentKind::MotionBlur, "motion_blur"},
    {AdjustmentKind::RadialBlur, "radial_blur"},
    {AdjustmentKind::SurfaceBlur, "surface_blur"},
    {AdjustmentKind::LensBlur, "lens_blur"},
    {AdjustmentKind::DofBlur, "dof_blur"},
    {AdjustmentKind::ShadowsHighlights, "shadows_highlights"},  // S34 colour repairs
    {AdjustmentKind::Defringe, "defringe"},
    {AdjustmentKind::MatteRemoval, "matte_removal"},
    {AdjustmentKind::HazeRemoval, "haze_removal"},
    {AdjustmentKind::Sharpen, "sharpen"},              // S35 artistic / stylize family
    {AdjustmentKind::UnsharpMask, "unsharp_mask"},
    {AdjustmentKind::AddNoise, "add_noise"},
    {AdjustmentKind::Denoise, "denoise"},
    {AdjustmentKind::Pixelate, "pixelate"},
    {AdjustmentKind::Emboss, "emboss"},
    {AdjustmentKind::OilPaint, "oil_paint"},
    {AdjustmentKind::Wave, "wave"},
    {AdjustmentKind::Vignette, "vignette"},
    {AdjustmentKind::GradientMap, "gradient_map"},  // S34-a gallery remainder
    {AdjustmentKind::Vibrance, "vibrance"},
    {AdjustmentKind::PhotoFilter, "photo_filter"},
    {AdjustmentKind::HighPass, "high_pass"},
}};

namespace vc = core::vec;

constexpr std::array<std::pair<vc::Node::Type, const char*>, 3> kNodeTypeTokens{{
    {vc::Node::Type::Corner, "corner"},
    {vc::Node::Type::Smooth, "smooth"},
    {vc::Node::Type::Symmetric, "symmetric"},
}};
constexpr std::array<std::pair<vc::FillRule, const char*>, 2> kFillRuleTokens{{
    {vc::FillRule::NonZero, "nonzero"},
    {vc::FillRule::EvenOdd, "evenodd"},
}};
constexpr std::array<std::pair<vc::CornerStyle, const char*>, 4> kCornerStyleTokens{{
    {vc::CornerStyle::Round, "round"},
    {vc::CornerStyle::Inverse, "inverse"},
    {vc::CornerStyle::Bevel, "bevel"},
    {vc::CornerStyle::None, "none"},
}};
constexpr std::array<std::pair<vc::EllipseShape::ArcMode, const char*>, 3> kArcModeTokens{{
    {vc::EllipseShape::ArcMode::Open, "open"},
    {vc::EllipseShape::ArcMode::Chord, "chord"},
    {vc::EllipseShape::ArcMode::Pie, "pie"},
}};
constexpr std::array<std::pair<vc::LineShape::Paint, const char*>, 3> kLinePaintTokens{{
    {vc::LineShape::Paint::Solid, "solid"},
    {vc::LineShape::Paint::Hollow, "hollow"},
    {vc::LineShape::Paint::Outlined, "outlined"},
}};
constexpr std::array<std::pair<vc::CalloutShape::Body, const char*>, 2> kCalloutBodyTokens{{
    {vc::CalloutShape::Body::RoundedRect, "rounded_rect"},
    {vc::CalloutShape::Body::Ellipse, "ellipse"},
}};
constexpr std::array<std::pair<vc::CalloutShape::Tail, const char*>, 2> kCalloutTailTokens{{
    {vc::CalloutShape::Tail::Pointer, "pointer"},
    {vc::CalloutShape::Tail::Bubbles, "bubbles"},
}};
constexpr std::array<std::pair<vc::BannerShape::Style, const char*>, 2> kBannerStyleTokens{{
    {vc::BannerShape::Style::Chevron, "chevron"},
    {vc::BannerShape::Style::Banner, "banner"},
}};
// Live booleans (S28). "add" is the menu's word for Union; the wire keeps the menu's spelling.
constexpr std::array<std::pair<vc::BoolOp, const char*>, 4> kBoolOpTokens{{
    {vc::BoolOp::Union, "add"},
    {vc::BoolOp::Subtract, "subtract"},
    {vc::BoolOp::Intersect, "intersect"},
    {vc::BoolOp::Exclude, "exclude"},
}};
constexpr std::array<std::pair<vc::GradientType, const char*>, 3> kGradientTypeTokens{{
    {vc::GradientType::Linear, "linear"},
    {vc::GradientType::Radial, "radial"},
    {vc::GradientType::Conic, "conic"},
}};
constexpr std::array<std::pair<vc::SpreadMethod, const char*>, 3> kSpreadTokens{{
    {vc::SpreadMethod::Pad, "pad"},
    {vc::SpreadMethod::Repeat, "repeat"},
    {vc::SpreadMethod::Reflect, "reflect"},
}};
// The gradient's banding control (S22), carried exactly like the spread above -- the difference is
// that "dither" is OPTIONAL on the wire: it is written only when it is not None, so every document
// written before S22 loads (as None) and every gradient without one serialises byte-identically.
constexpr std::array<std::pair<vc::DitherKind, const char*>, 4> kDitherTokens{{
    {vc::DitherKind::None, "none"},
    {vc::DitherKind::Ordered, "ordered"},
    {vc::DitherKind::BlueNoise, "blue"},
    {vc::DitherKind::Noise, "noise"},
}};
constexpr std::array<std::pair<vc::LineCap, const char*>, 3> kCapTokens{{
    {vc::LineCap::Butt, "butt"},
    {vc::LineCap::Round, "round"},
    {vc::LineCap::Square, "square"},
}};
constexpr std::array<std::pair<vc::LineJoin, const char*>, 3> kJoinTokens{{
    {vc::LineJoin::Miter, "miter"},
    {vc::LineJoin::Round, "round"},
    {vc::LineJoin::Bevel, "bevel"},
}};
constexpr std::array<std::pair<vc::StrokeAlign, const char*>, 3> kStrokeAlignTokens{{
    {vc::StrokeAlign::Center, "center"},
    {vc::StrokeAlign::Inside, "inside"},
    {vc::StrokeAlign::Outside, "outside"},
}};
constexpr std::array<std::pair<vc::Object::PaintOrder, const char*>, 2> kPaintOrderTokens{{
    {vc::Object::PaintOrder::FillThenStroke, "fill_stroke"},
    {vc::Object::PaintOrder::StrokeThenFill, "stroke_fill"},
}};
constexpr std::array<std::pair<vc::ProceduralPattern::Kind, const char*>, 25> kPatternKindTokens{{
    {vc::ProceduralPattern::Kind::Dots, "dots"},
    {vc::ProceduralPattern::Kind::Grid, "grid"},
    {vc::ProceduralPattern::Kind::Lines, "lines"},
    {vc::ProceduralPattern::Kind::Hatch, "hatch"},
    {vc::ProceduralPattern::Kind::CrossHatch, "cross_hatch"},
    {vc::ProceduralPattern::Kind::Checker, "checker"},
    {vc::ProceduralPattern::Kind::Herringbone, "herringbone"},
    {vc::ProceduralPattern::Kind::Parquet, "parquet"},
    {vc::ProceduralPattern::Kind::Basketweave, "basketweave"},
    {vc::ProceduralPattern::Kind::Chevron, "chevron"},
    {vc::ProceduralPattern::Kind::Zigzag, "zigzag"},
    {vc::ProceduralPattern::Kind::Chainmail, "chainmail"},
    {vc::ProceduralPattern::Kind::Halftone, "halftone"},
    {vc::ProceduralPattern::Kind::Grain, "grain"},
    {vc::ProceduralPattern::Kind::Bricks, "bricks"},
    {vc::ProceduralPattern::Kind::Triangles, "triangles"},
    {vc::ProceduralPattern::Kind::Sawtooth, "sawtooth"},
    {vc::ProceduralPattern::Kind::Harlequin, "harlequin"},
    {vc::ProceduralPattern::Kind::Honeycomb, "honeycomb"},
    {vc::ProceduralPattern::Kind::Waves, "waves"},
    {vc::ProceduralPattern::Kind::Stars, "stars"},
    {vc::ProceduralPattern::Kind::StarAnise, "star_anise"},
    {vc::ProceduralPattern::Kind::Hearts, "hearts"},
    {vc::ProceduralPattern::Kind::Crosses, "crosses"},
    {vc::ProceduralPattern::Kind::Rings, "rings"},
}};

constexpr std::array<std::pair<core::StrokeEffect::Align, const char*>, 3> kFxAlignTokens{{
    {core::StrokeEffect::Align::Inside, "inside"},
    {core::StrokeEffect::Align::Center, "center"},
    {core::StrokeEffect::Align::Outside, "outside"},
}};
constexpr std::array<std::pair<core::GlowEffect::Source, const char*>, 2> kGlowSourceTokens{{
    {core::GlowEffect::Source::Edge, "edge"},
    {core::GlowEffect::Source::Center, "center"},
}};
constexpr std::array<std::pair<core::BevelEffect::Style, const char*>, 4> kBevelStyleTokens{{
    {core::BevelEffect::Style::OuterBevel, "outer_bevel"},
    {core::BevelEffect::Style::InnerBevel, "inner_bevel"},
    {core::BevelEffect::Style::Emboss, "emboss"},
    {core::BevelEffect::Style::PillowEmboss, "pillow_emboss"},
}};

namespace tx = core::text;

constexpr std::array<std::pair<tx::Kerning, const char*>, 3> kKerningTokens{{
    {tx::Kerning::Metric, "metric"},
    {tx::Kerning::Optical, "optical"},
    {tx::Kerning::None, "none"},
}};
constexpr std::array<std::pair<tx::Paragraph::Align, const char*>, 4> kParaAlignTokens{{
    {tx::Paragraph::Align::Left, "left"},
    {tx::Paragraph::Align::Center, "center"},
    {tx::Paragraph::Align::Right, "right"},
    {tx::Paragraph::Align::Justify, "justify"},
}};
constexpr std::array<std::pair<tx::Paragraph::Direction, const char*>, 3> kParaDirTokens{{
    {tx::Paragraph::Direction::Auto, "auto"},
    {tx::Paragraph::Direction::LTR, "ltr"},
    {tx::Paragraph::Direction::RTL, "rtl"},
}};
constexpr std::array<std::pair<tx::TextFrame, const char*>, 2> kFrameTokens{{
    {tx::TextFrame::Point, "point"},
    {tx::TextFrame::Area, "area"},
}};
constexpr std::array<std::pair<tx::AntiAlias, const char*>, 3> kAaTokens{{
    {tx::AntiAlias::None, "none"},
    {tx::AntiAlias::Grayscale, "grayscale"},
    {tx::AntiAlias::Subpixel, "subpixel"},
}};
constexpr std::array<std::pair<tx::WritingMode, const char*>, 3> kWritingModeTokens{{
    {tx::WritingMode::HorizontalTB, "horizontal_tb"},
    {tx::WritingMode::VerticalRL, "vertical_rl"},
    {tx::WritingMode::VerticalLR, "vertical_lr"},
}};
constexpr std::array<std::pair<tx::TextOrientation, const char*>, 2> kOrientationTokens{{
    {tx::TextOrientation::Mixed, "mixed"},
    {tx::TextOrientation::Upright, "upright"},
}};
constexpr std::array<std::pair<tx::Bevel::Profile, const char*>, 4> kBevelProfileTokens{{
    {tx::Bevel::Profile::Flat, "flat"},
    {tx::Bevel::Profile::Round, "round"},
    {tx::Bevel::Profile::Convex, "convex"},
    {tx::Bevel::Profile::Concave, "concave"},
}};

// ---- primitives ------------------------------------------------------------------------------

json vec2ToJson(common::Vec2 v) {
    return json::array({v.x, v.y});
}

std::optional<common::Vec2> vec2FromJson(const json& j) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number())
        return std::nullopt;
    return common::Vec2{j[0].get<double>(), j[1].get<double>()};
}

json colorToJson(common::ColorF c) {
    return json::array({c.r, c.g, c.b, c.a});
}

std::optional<common::ColorF> colorFromJson(const json& j) {
    if (!j.is_array() || j.size() != 4)
        return std::nullopt;
    common::ColorF c;
    float* ch[4] = {&c.r, &c.g, &c.b, &c.a};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!j[i].is_number())
            return std::nullopt;
        *ch[i] = j[i].get<float>();
    }
    return c;
}

std::optional<common::ColorF> colorField(const json& j, const char* k) {
    const auto it = j.find(k);
    if (it == j.end())
        return std::nullopt;
    return colorFromJson(*it);
}

// ---- base64 (ImagePattern tiles ride inline: small, immutable bitmaps) ------------------------

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::uint8_t* data, std::size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const std::uint32_t b0 = data[i];
        const std::uint32_t b1 = i + 1 < n ? data[i + 1] : 0;
        const std::uint32_t b2 = i + 2 < n ? data[i + 2] : 0;
        const std::uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? kB64[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? kB64[v & 63] : '=');
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64Decode(const std::string& s) {
    if (s.size() % 4 != 0)
        return std::nullopt;
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(s.size() / 4 * 3);
    for (std::size_t i = 0; i < s.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const char c = s[i + k];
            if (c == '=' && i + 4 == s.size() && k >= 2) {
                v[k] = 0;
                ++pad;
            } else {
                v[k] = value(c);
                if (v[k] < 0 || pad > 0)
                    return std::nullopt; // garbage, or data after padding
            }
        }
        const std::uint32_t b = (static_cast<std::uint32_t>(v[0]) << 18) |
                                (static_cast<std::uint32_t>(v[1]) << 12) |
                                (static_cast<std::uint32_t>(v[2]) << 6) |
                                static_cast<std::uint32_t>(v[3]);
        out.push_back(static_cast<std::uint8_t>(b >> 16));
        if (pad < 2)
            out.push_back(static_cast<std::uint8_t>(b >> 8));
        if (pad < 1)
            out.push_back(static_cast<std::uint8_t>(b));
    }
    return out;
}

// ---- paint family ----------------------------------------------------------------------------

json patternToJson(const vc::Pattern& p) {
    if (const auto* proc = std::get_if<vc::ProceduralPattern>(&p)) {
        return json{{"kind", "procedural"},
                    {"motif", tokenOf(kPatternKindTokens, proc->kind)},
                    {"fg", colorToJson(proc->fg)},
                    {"bg", colorToJson(proc->bg)},
                    {"scale", proc->scale},
                    {"angle", proc->angleDeg},
                    {"offset", proc->offset},
                    {"weight", proc->weight},
                    {"spacing", proc->spacing},
                    {"anchor_canvas", proc->anchorToCanvas}};
    }
    const auto& img = std::get<vc::ImagePattern>(p);
    json tile = nullptr;
    if (img.tile != nullptr && img.tile->width > 0 && img.tile->height > 0) {
        tile = json{{"w", img.tile->width},
                    {"h", img.tile->height},
                    {"rgba", base64Encode(img.tile->rgba.data(), img.tile->rgba.size())}};
    }
    return json{{"kind", "image"},
                {"tile", std::move(tile)},
                {"scale", img.scale},
                {"angle", img.angleDeg},
                {"offset", vec2ToJson(img.offset)}};
}

std::optional<vc::Pattern> patternFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    std::string kind;
    if (!getStr(j, "kind", kind))
        return std::nullopt;
    if (kind == "procedural") {
        vc::ProceduralPattern p;
        const auto motif = enumField(kPatternKindTokens, j, "motif");
        const auto fg = colorField(j, "fg");
        const auto bg = colorField(j, "bg");
        if (!motif || !fg || !bg || !getF(j, "scale", p.scale) || !getF(j, "angle", p.angleDeg) ||
            !getF(j, "offset", p.offset) || !getF(j, "weight", p.weight) ||
            !getF(j, "spacing", p.spacing) || !getB(j, "anchor_canvas", p.anchorToCanvas))
            return std::nullopt;
        p.kind = *motif;
        p.fg = *fg;
        p.bg = *bg;
        return vc::Pattern{p};
    }
    if (kind == "image") {
        vc::ImagePattern p;
        const auto off = j.contains("offset") ? vec2FromJson(j["offset"])
                                              : std::optional<common::Vec2>{};
        if (!off || !getF(j, "scale", p.scale) || !getF(j, "angle", p.angleDeg))
            return std::nullopt;
        p.offset = *off;
        const auto tileIt = j.find("tile");
        if (tileIt == j.end())
            return std::nullopt;
        if (tileIt->is_object()) {
            std::uint64_t w = 0, h = 0;
            std::string b64;
            if (!getU64(*tileIt, "w", w) || !getU64(*tileIt, "h", h) ||
                !getStr(*tileIt, "rgba", b64))
                return std::nullopt;
            // A pattern tile is a small brush-tile bitmap; 4096px on a side is already absurd,
            // and the cap keeps a hostile manifest from asking for gigabytes.
            if (w == 0 || h == 0 || w > 4096 || h > 4096)
                return std::nullopt;
            auto bytes = base64Decode(b64);
            if (!bytes.has_value() || bytes->size() != w * h * 4)
                return std::nullopt;
            common::Image tile(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
            tile.rgba = std::move(*bytes);
            p.tile = std::make_shared<const common::Image>(std::move(tile));
        } else if (!tileIt->is_null()) {
            return std::nullopt;
        }
        return vc::Pattern{std::move(p)};
    }
    return std::nullopt;
}

json paintToJson(const vc::Paint& p) {
    if (std::holds_alternative<vc::NoPaint>(p))
        return json{{"type", "none"}};
    if (const auto* s = std::get_if<vc::SolidPaint>(&p))
        return json{{"type", "solid"}, {"color", colorToJson(s->color)}};
    if (const auto* g = std::get_if<vc::Gradient>(&p)) {
        json stops = json::array();
        for (const vc::GradientStop& st : g->stops) {
            // The trailing midpoint (S22 blend curve) is written only when it is off the 0.5 default,
            // so gradients without blend curves serialise byte-identically to pre-S22.
            json arr = json::array({st.offset, st.color.r, st.color.g, st.color.b, st.color.a});
            if (std::abs(st.midpoint - 0.5) > 1e-9)
                arr.push_back(st.midpoint);
            stops.push_back(std::move(arr));
        }
        json out{{"type", "gradient"},
                 {"gradient", tokenOf(kGradientTypeTokens, g->type)},
                 {"stops", std::move(stops)},
                 {"transform", affineToJson(g->transform)},
                 {"spread", tokenOf(kSpreadTokens, g->spread)}};
        if (g->dither != vc::DitherKind::None)  // omitted at the default, so pre-S22 files match
            out["dither"] = tokenOf(kDitherTokens, g->dither);
        return out;
    }
    return json{{"type", "pattern"}, {"pattern", patternToJson(std::get<vc::Pattern>(p))}};
}

std::optional<vc::Paint> paintFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    std::string type;
    if (!getStr(j, "type", type))
        return std::nullopt;
    if (type == "none")
        return vc::Paint{vc::NoPaint{}};
    if (type == "solid") {
        const auto c = colorField(j, "color");
        if (!c)
            return std::nullopt;
        return vc::Paint{vc::SolidPaint{*c}};
    }
    if (type == "gradient") {
        vc::Gradient g;
        const auto type2 = enumField(kGradientTypeTokens, j, "gradient");
        const auto spread = enumField(kSpreadTokens, j, "spread");
        const json* stops = getArr(j, "stops");
        const auto tf = j.contains("transform") ? affineFromJson(j["transform"])
                                                : std::optional<common::Affine2D>{};
        if (!type2 || !spread || stops == nullptr || !tf)
            return std::nullopt;
        g.type = *type2;
        g.spread = *spread;
        g.transform = *tf;
        // Optional since S22: absent (every older file) -> None, the exact ramp. An unrecognised
        // token is an error like every other enum field, not a silent fall-back to None.
        if (j.contains("dither")) {
            const auto dither = enumField(kDitherTokens, j, "dither");
            if (!dither)
                return std::nullopt;
            g.dither = *dither;
        }
        for (const json& s : *stops) {
            // [offset, r, g, b, a] and, since S22, an OPTIONAL 6th element = the blend-curve
            // midpoint (absent -> 0.5, a straight linear blend, so pre-S22 files load unchanged).
            if (!s.is_array() || (s.size() != 5 && s.size() != 6))
                return std::nullopt;
            for (const json& n : s)
                if (!n.is_number())
                    return std::nullopt;
            vc::GradientStop st{s[0].get<double>(),
                                common::ColorF{s[1].get<float>(), s[2].get<float>(),
                                               s[3].get<float>(), s[4].get<float>()}};
            if (s.size() == 6)
                st.midpoint = s[5].get<double>();
            g.stops.push_back(st);
        }
        return vc::Paint{std::move(g)};
    }
    if (type == "pattern") {
        const json* p = getObj(j, "pattern");
        if (p == nullptr)
            return std::nullopt;
        auto pattern = patternFromJson(*p);
        if (!pattern)
            return std::nullopt;
        return vc::Paint{std::move(*pattern)};
    }
    return std::nullopt;
}

std::optional<vc::Paint> paintField(const json& j, const char* k) {
    const json* p = getObj(j, k);
    if (p == nullptr)
        return std::nullopt;
    return paintFromJson(*p);
}

// ---- stroke ------------------------------------------------------------------------------------

json strokeToJson(const vc::Stroke& s) {
    return json{{"paint", paintToJson(s.paint)},
                {"width", s.width},
                {"miter", s.miterLimit},
                {"dash_offset", s.dashOffset},
                {"cap", tokenOf(kCapTokens, s.cap)},
                {"join", tokenOf(kJoinTokens, s.join)},
                {"align", tokenOf(kStrokeAlignTokens, s.align)},
                {"dash", s.dashArray},
                {"enabled", s.enabled}};
}

std::optional<vc::Stroke> strokeFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    vc::Stroke s;
    auto paint = paintField(j, "paint");
    const auto cap = enumField(kCapTokens, j, "cap");
    const auto join = enumField(kJoinTokens, j, "join");
    const auto align = enumField(kStrokeAlignTokens, j, "align");
    const json* dash = getArr(j, "dash");
    if (!paint || !cap || !join || !align || dash == nullptr || !getD(j, "width", s.width) ||
        !getD(j, "miter", s.miterLimit) || !getD(j, "dash_offset", s.dashOffset) ||
        !getB(j, "enabled", s.enabled))
        return std::nullopt;
    s.paint = std::move(*paint);
    s.cap = *cap;
    s.join = *join;
    s.align = *align;
    for (const json& d : *dash) {
        if (!d.is_number())
            return std::nullopt;
        s.dashArray.push_back(d.get<double>());
    }
    return s;
}

// ---- geometry ----------------------------------------------------------------------------------

json subPathToJson(const vc::SubPath& sp) {
    json nodes = json::array();
    for (const vc::Node& n : sp.nodes)
        nodes.push_back(json::array({n.anchor.x, n.anchor.y, n.inHandle.x, n.inHandle.y,
                                     n.outHandle.x, n.outHandle.y,
                                     tokenOf(kNodeTypeTokens, n.type)}));
    return json{{"closed", sp.closed}, {"nodes", std::move(nodes)}};
}

std::optional<vc::SubPath> subPathFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    vc::SubPath sp;
    const json* nodes = getArr(j, "nodes");
    if (nodes == nullptr || !getB(j, "closed", sp.closed))
        return std::nullopt;
    for (const json& n : *nodes) {
        if (!n.is_array() || n.size() != 7 || !n[6].is_string())
            return std::nullopt;
        for (std::size_t i = 0; i < 6; ++i)
            if (!n[i].is_number())
                return std::nullopt;
        const auto type = enumOf(kNodeTypeTokens, n[6].get<std::string>());
        if (!type)
            return std::nullopt;
        sp.nodes.push_back({{n[0].get<double>(), n[1].get<double>()},
                            {n[2].get<double>(), n[3].get<double>()},
                            {n[4].get<double>(), n[5].get<double>()},
                            *type});
    }
    return sp;
}

json pathToJson(const vc::Path& path) {
    json subs = json::array();
    for (const vc::SubPath& sp : path.subpaths)
        subs.push_back(subPathToJson(sp));
    return json{{"type", "path"},
                {"fill_rule", tokenOf(kFillRuleTokens, path.fillRule)},
                {"subpaths", std::move(subs)}};
}

json geometryToJson(const vc::Geometry& g) {
    if (const auto* path = std::get_if<vc::Path>(&g))
        return pathToJson(*path);
    // ---- live boolean compound (S28), written FORWARD-COMPATIBLY ----
    // The wire spelling is a "path" carrying the BAKED outline plus a "boolean" side-car. That
    // direction is deliberate: the reader dispatches on "type", so a build that predates booleans
    // meeting a "type":"boolean" would reject the payload, and docio's tolerance rule then inserts
    // an EMPTY vector layer (docio.cpp) -- re-saving from that build would destroy the shape for
    // good. Written this way, an old build reads a shape it fully understands (it just stops being
    // live), a new build prefers the side-car, and the format version does NOT have to move.
    if (const auto* compound = std::get_if<vc::BooleanCompound>(&g)) {
        json kids = json::array();
        for (const vc::Object& child : compound->children)
            kids.push_back(vectorObjectToJson(child));
        json out = pathToJson(vc::bakedBooleanPath(*compound));
        out["boolean"] = json{{"op", tokenOf(kBoolOpTokens, compound->op)},
                              {"children", std::move(kids)}};
        return out;
    }
    const auto& shape = std::get<vc::ParametricShape>(g);
    if (const auto* r = std::get_if<vc::RectShape>(&shape)) {
        json styles = json::array();
        for (const vc::CornerStyle st : r->cornerStyle)
            styles.push_back(tokenOf(kCornerStyleTokens, st));
        return json{{"type", "rect"},
                    {"size", vec2ToJson(r->size)},
                    {"radius", r->cornerRadius},
                    {"corner_style", std::move(styles)}};
    }
    if (const auto* e = std::get_if<vc::EllipseShape>(&shape)) {
        return json{{"type", "ellipse"},
                    {"radii", vec2ToJson(e->radii)},
                    {"start", e->startAngle},
                    {"end", e->endAngle},
                    {"arc_mode", tokenOf(kArcModeTokens, e->arcMode)}};
    }
    if (const auto* p = std::get_if<vc::PolygonShape>(&shape)) {
        return json{{"type", "polygon"},
                    {"sides", p->sides},
                    {"radius", p->radius},
                    {"corner_radius", p->cornerRadius},
                    {"corner_style", tokenOf(kCornerStyleTokens, p->cornerStyle)}};
    }
    if (const auto* s = std::get_if<vc::StarShape>(&shape)) {
        return json{{"type", "star"},
                    {"points", s->points},
                    {"outer", s->outerRadius},
                    {"inner", s->innerRadius},
                    {"point_radius", s->pointRadius},
                    {"valley_radius", s->valleyRadius}};
    }
    if (const auto* l = std::get_if<vc::LineShape>(&shape)) {
        return json{{"type", "line"},
                    {"a", vec2ToJson(l->a)},
                    {"b", vec2ToJson(l->b)},
                    {"paint_mode", tokenOf(kLinePaintTokens, l->paint)},
                    {"border_width", l->borderWidth},
                    {"bend", vec2ToJson(l->bend)}};
    }
    // ---- the widened shape library (S26-c) ----
    if (const auto* c = std::get_if<vc::CalloutShape>(&shape)) {
        return json{{"type", "callout"},
                    {"size", vec2ToJson(c->size)},
                    {"body", tokenOf(kCalloutBodyTokens, c->body)},
                    {"corner_radius", c->cornerRadius},
                    {"tail_kind", tokenOf(kCalloutTailTokens, c->tail)},
                    {"tail_angle", c->tailAngle},
                    {"tail_length", c->tailLength},
                    {"tail_width", c->tailWidth},
                    {"tail_skew", c->tailSkew},
                    {"bubbles", c->bubbleCount}};
    }
    if (const auto* a = std::get_if<vc::ArrowShape>(&shape)) {
        return json{{"type", "arrow"},
                    {"size", vec2ToJson(a->size)},
                    {"shaft", a->shaftRatio},
                    {"head", a->headRatio},
                    {"notch", a->notchRatio},
                    {"double_headed", a->doubleHeaded}};
    }
    if (const auto* r = std::get_if<vc::RingShape>(&shape)) {
        return json{{"type", "ring"},
                    {"radii", vec2ToJson(r->radii)},
                    {"inner", r->innerRatio},
                    {"start", r->startAngle},
                    {"end", r->endAngle}};
    }
    if (const auto* x = std::get_if<vc::CrossShape>(&shape)) {
        return json{{"type", "cross"},
                    {"size", vec2ToJson(x->size)},
                    {"arm", x->armRatio},
                    {"corner_radius", x->cornerRadius},
                    {"corner_style", tokenOf(kCornerStyleTokens, x->cornerStyle)}};
    }
    if (const auto* hb = std::get_if<vc::HeartShape>(&shape)) {
        return json{{"type", "heart"},
                    {"size", vec2ToJson(hb->size)},
                    {"lobe", hb->lobe},
                    {"cleft", hb->cleft}};
    }
    const auto& b = std::get<vc::BannerShape>(shape);
    return json{{"type", "banner"},
                {"size", vec2ToJson(b.size)},
                {"style", tokenOf(kBannerStyleTokens, b.style)},
                {"point", b.pointRatio},
                {"notch_tail", b.notchTail},
                {"corner_radius", b.cornerRadius}};
}

std::optional<vc::Geometry> geometryFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    std::string type;
    if (!getStr(j, "type", type))
        return std::nullopt;
    if (type == "path") {
        // A live boolean compound writes itself as a "path" (the baked outline) plus a "boolean"
        // side-car -- see geometryToJson. Prefer the side-car when it is there; the baked subpaths
        // alongside it exist for builds that predate S28. Strict, per this file's contract: a
        // MALFORMED side-car is a reject, not a quiet downgrade to the baked outline. Silently
        // dropping the operands would look like a successful load and lose live editability with
        // no counter moving.
        if (const json* boolJ = getObj(j, "boolean")) {
            vc::BooleanCompound compound;
            const auto op = enumField(kBoolOpTokens, *boolJ, "op");
            const json* kids = getArr(*boolJ, "children");
            if (!op || kids == nullptr)
                return std::nullopt;
            compound.op = *op;
            for (const json& kid : *kids) {
                auto child = vectorObjectFromJson(kid);
                if (!child)
                    return std::nullopt;
                compound.children.push_back(std::move(*child));
            }
            return vc::Geometry{std::move(compound)};
        }
        vc::Path path;
        const auto rule = enumField(kFillRuleTokens, j, "fill_rule");
        const json* subs = getArr(j, "subpaths");
        if (!rule || subs == nullptr)
            return std::nullopt;
        path.fillRule = *rule;
        for (const json& sp : *subs) {
            auto sub = subPathFromJson(sp);
            if (!sub)
                return std::nullopt;
            path.subpaths.push_back(std::move(*sub));
        }
        return vc::Geometry{std::move(path)};
    }
    if (type == "rect") {
        vc::RectShape r;
        const auto size = j.contains("size") ? vec2FromJson(j["size"])
                                             : std::optional<common::Vec2>{};
        const json* radius = getArr(j, "radius");
        const json* styles = getArr(j, "corner_style");
        if (!size || radius == nullptr || radius->size() != 4 || styles == nullptr ||
            styles->size() != 4)
            return std::nullopt;
        r.size = *size;
        for (std::size_t i = 0; i < 4; ++i) {
            if (!(*radius)[i].is_number() || !(*styles)[i].is_string())
                return std::nullopt;
            r.cornerRadius[i] = (*radius)[i].get<double>();
            const auto st = enumOf(kCornerStyleTokens, (*styles)[i].get<std::string>());
            if (!st)
                return std::nullopt;
            r.cornerStyle[i] = *st;
        }
        return vc::Geometry{vc::ParametricShape{r}};
    }
    if (type == "ellipse") {
        vc::EllipseShape e;
        const auto radii = j.contains("radii") ? vec2FromJson(j["radii"])
                                               : std::optional<common::Vec2>{};
        const auto mode = enumField(kArcModeTokens, j, "arc_mode");
        if (!radii || !mode || !getD(j, "start", e.startAngle) || !getD(j, "end", e.endAngle))
            return std::nullopt;
        e.radii = *radii;
        e.arcMode = *mode;
        return vc::Geometry{vc::ParametricShape{e}};
    }
    if (type == "polygon") {
        vc::PolygonShape p;
        const auto style = enumField(kCornerStyleTokens, j, "corner_style");
        if (!style || !getI(j, "sides", p.sides) || !getD(j, "radius", p.radius) ||
            !getD(j, "corner_radius", p.cornerRadius))
            return std::nullopt;
        p.cornerStyle = *style;
        return vc::Geometry{vc::ParametricShape{p}};
    }
    if (type == "star") {
        vc::StarShape s;
        if (!getI(j, "points", s.points) || !getD(j, "outer", s.outerRadius) ||
            !getD(j, "inner", s.innerRadius) || !getD(j, "point_radius", s.pointRadius) ||
            !getD(j, "valley_radius", s.valleyRadius))
            return std::nullopt;
        return vc::Geometry{vc::ParametricShape{s}};
    }
    if (type == "line") {
        vc::LineShape l;
        const auto a = j.contains("a") ? vec2FromJson(j["a"]) : std::optional<common::Vec2>{};
        const auto b = j.contains("b") ? vec2FromJson(j["b"]) : std::optional<common::Vec2>{};
        const auto bend = j.contains("bend") ? vec2FromJson(j["bend"])
                                             : std::optional<common::Vec2>{};
        const auto mode = enumField(kLinePaintTokens, j, "paint_mode");
        if (!a || !b || !bend || !mode || !getD(j, "border_width", l.borderWidth))
            return std::nullopt;
        l.a = *a;
        l.b = *b;
        l.bend = *bend;
        l.paint = *mode;
        return vc::Geometry{vc::ParametricShape{l}};
    }
    // ---- the widened shape library (S26-c) ----
    if (type == "callout") {
        vc::CalloutShape c;
        const auto size = j.contains("size") ? vec2FromJson(j["size"]) : std::optional<common::Vec2>{};
        const auto body = enumField(kCalloutBodyTokens, j, "body");
        const auto tail = enumField(kCalloutTailTokens, j, "tail_kind");
        if (!size || !body || !tail || !getD(j, "corner_radius", c.cornerRadius) ||
            !getD(j, "tail_angle", c.tailAngle) || !getD(j, "tail_length", c.tailLength) ||
            !getD(j, "tail_width", c.tailWidth) || !getD(j, "tail_skew", c.tailSkew) ||
            !getI(j, "bubbles", c.bubbleCount))
            return std::nullopt;
        c.size = *size;
        c.body = *body;
        c.tail = *tail;
        return vc::Geometry{vc::ParametricShape{c}};
    }
    if (type == "arrow") {
        vc::ArrowShape a;
        const auto size = j.contains("size") ? vec2FromJson(j["size"]) : std::optional<common::Vec2>{};
        if (!size || !getD(j, "shaft", a.shaftRatio) || !getD(j, "head", a.headRatio) ||
            !getD(j, "notch", a.notchRatio) || !getB(j, "double_headed", a.doubleHeaded))
            return std::nullopt;
        a.size = *size;
        return vc::Geometry{vc::ParametricShape{a}};
    }
    if (type == "ring") {
        vc::RingShape r;
        const auto radii = j.contains("radii") ? vec2FromJson(j["radii"])
                                               : std::optional<common::Vec2>{};
        if (!radii || !getD(j, "inner", r.innerRatio) || !getD(j, "start", r.startAngle) ||
            !getD(j, "end", r.endAngle))
            return std::nullopt;
        r.radii = *radii;
        return vc::Geometry{vc::ParametricShape{r}};
    }
    if (type == "cross") {
        vc::CrossShape x;
        const auto size = j.contains("size") ? vec2FromJson(j["size"]) : std::optional<common::Vec2>{};
        const auto style = enumField(kCornerStyleTokens, j, "corner_style");
        if (!size || !style || !getD(j, "arm", x.armRatio) ||
            !getD(j, "corner_radius", x.cornerRadius))
            return std::nullopt;
        x.size = *size;
        x.cornerStyle = *style;
        return vc::Geometry{vc::ParametricShape{x}};
    }
    if (type == "heart") {
        vc::HeartShape hb;
        const auto size = j.contains("size") ? vec2FromJson(j["size"]) : std::optional<common::Vec2>{};
        if (!size || !getD(j, "lobe", hb.lobe) || !getD(j, "cleft", hb.cleft))
            return std::nullopt;
        hb.size = *size;
        return vc::Geometry{vc::ParametricShape{hb}};
    }
    if (type == "banner") {
        vc::BannerShape b;
        const auto size = j.contains("size") ? vec2FromJson(j["size"]) : std::optional<common::Vec2>{};
        const auto style = enumField(kBannerStyleTokens, j, "style");
        if (!size || !style || !getD(j, "point", b.pointRatio) ||
            !getB(j, "notch_tail", b.notchTail) || !getD(j, "corner_radius", b.cornerRadius))
            return std::nullopt;
        b.size = *size;
        b.style = *style;
        return vc::Geometry{vc::ParametricShape{b}};
    }
    return std::nullopt;
}

} // namespace

// ---- shared pieces (declared in docjson.hpp) ---------------------------------------------------

json affineToJson(const common::Affine2D& t) {
    return json::array({t.m00, t.m01, t.m02, t.m10, t.m11, t.m12});
}

std::optional<common::Affine2D> affineFromJson(const json& j) {
    if (!j.is_array() || j.size() != 6)
        return std::nullopt;
    common::Affine2D t;
    double* m[6] = {&t.m00, &t.m01, &t.m02, &t.m10, &t.m11, &t.m12};
    for (std::size_t i = 0; i < 6; ++i) {
        if (!j[i].is_number())
            return std::nullopt;
        *m[i] = j[i].get<double>();
        if (!std::isfinite(*m[i]))
            return std::nullopt;
    }
    return t;
}

const char* blendModeToken(core::BlendMode m) {
    return tokenOf(kBlendTokens, m);
}

std::optional<core::BlendMode> blendModeFromToken(const std::string& s) {
    return enumOf(kBlendTokens, s);
}

const char* adjustmentKindToken(core::AdjustmentKind k) {
    return tokenOf(kAdjustmentTokens, k);
}

std::optional<core::AdjustmentKind> adjustmentKindFromToken(const std::string& s) {
    return enumOf(kAdjustmentTokens, s);
}

// ---- vector object -----------------------------------------------------------------------------

json vectorObjectToJson(const vc::Object& o) {
    return json{{"geometry", geometryToJson(o.geometry)},
                {"fill", paintToJson(o.fill)},
                {"stroke", strokeToJson(o.stroke)},
                {"paint_order", tokenOf(kPaintOrderTokens, o.paintOrder)}};
}

std::optional<vc::Object> vectorObjectFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    vc::Object o;
    const json* geom = getObj(j, "geometry");
    const json* strokeJ = getObj(j, "stroke");
    auto fill = paintField(j, "fill");
    const auto order = enumField(kPaintOrderTokens, j, "paint_order");
    if (geom == nullptr || strokeJ == nullptr || !fill || !order)
        return std::nullopt;
    auto geometry = geometryFromJson(*geom);
    auto stroke = strokeFromJson(*strokeJ);
    if (!geometry || !stroke)
        return std::nullopt;
    o.geometry = std::move(*geometry);
    o.fill = std::move(*fill);
    o.stroke = std::move(*stroke);
    o.paintOrder = *order;
    return o;
}

// ---- layer effects -----------------------------------------------------------------------------

namespace {

json shadowToJson(const core::ShadowEffect& s) {
    return json{{"color", colorToJson(s.color)}, {"opacity", s.opacity},
                {"blend", blendModeToken(s.blend)}, {"angle", s.angleDeg},
                {"distance", s.distance}, {"spread", s.spread},
                {"size", s.size}, {"enabled", s.enabled}};
}

std::optional<core::ShadowEffect> shadowFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    core::ShadowEffect s;
    const auto color = colorField(j, "color");
    std::string blend;
    if (!color || !getStr(j, "blend", blend) || !getF(j, "opacity", s.opacity) ||
        !getF(j, "angle", s.angleDeg) || !getF(j, "distance", s.distance) ||
        !getF(j, "spread", s.spread) || !getF(j, "size", s.size) ||
        !getB(j, "enabled", s.enabled))
        return std::nullopt;
    const auto mode = blendModeFromToken(blend);
    if (!mode)
        return std::nullopt;
    s.color = *color;
    s.blend = *mode;
    return s;
}

json glowToJson(const core::GlowEffect& g) {
    return json{{"paint", paintToJson(g.paint)}, {"opacity", g.opacity},
                {"blend", blendModeToken(g.blend)}, {"choke", g.choke},
                {"size", g.size}, {"source", tokenOf(kGlowSourceTokens, g.source)},
                {"enabled", g.enabled}};
}

std::optional<core::GlowEffect> glowFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    core::GlowEffect g;
    auto paint = paintField(j, "paint");
    const auto source = enumField(kGlowSourceTokens, j, "source");
    std::string blend;
    if (!paint || !source || !getStr(j, "blend", blend) || !getF(j, "opacity", g.opacity) ||
        !getF(j, "choke", g.choke) || !getF(j, "size", g.size) || !getB(j, "enabled", g.enabled))
        return std::nullopt;
    const auto mode = blendModeFromToken(blend);
    if (!mode)
        return std::nullopt;
    g.paint = std::move(*paint);
    g.source = *source;
    g.blend = *mode;
    return g;
}

json overlayToJson(const core::OverlayEffect& o) {
    return json{{"paint", paintToJson(o.paint)}, {"blend", blendModeToken(o.blend)},
                {"opacity", o.opacity}, {"enabled", o.enabled}};
}

std::optional<core::OverlayEffect> overlayFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    core::OverlayEffect o;
    auto paint = paintField(j, "paint");
    std::string blend;
    if (!paint || !getStr(j, "blend", blend) || !getF(j, "opacity", o.opacity) ||
        !getB(j, "enabled", o.enabled))
        return std::nullopt;
    const auto mode = blendModeFromToken(blend);
    if (!mode)
        return std::nullopt;
    o.paint = std::move(*paint);
    o.blend = *mode;
    return o;
}

json strokeEffectToJson(const core::StrokeEffect& s) {
    return json{{"width", s.width}, {"align", tokenOf(kFxAlignTokens, s.align)},
                {"paint", paintToJson(s.paint)}, {"blend", blendModeToken(s.blend)},
                {"opacity", s.opacity}, {"enabled", s.enabled}};
}

std::optional<core::StrokeEffect> strokeEffectFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    core::StrokeEffect s;
    auto paint = paintField(j, "paint");
    const auto align = enumField(kFxAlignTokens, j, "align");
    std::string blend;
    if (!paint || !align || !getStr(j, "blend", blend) || !getF(j, "width", s.width) ||
        !getF(j, "opacity", s.opacity) || !getB(j, "enabled", s.enabled))
        return std::nullopt;
    const auto mode = blendModeFromToken(blend);
    if (!mode)
        return std::nullopt;
    s.paint = std::move(*paint);
    s.align = *align;
    s.blend = *mode;
    return s;
}

json satinToJson(const core::SatinEffect& s) {
    return json{{"color", colorToJson(s.color)}, {"opacity", s.opacity},
                {"blend", blendModeToken(s.blend)}, {"angle", s.angleDeg},
                {"distance", s.distance}, {"size", s.size},
                {"invert", s.invert}, {"enabled", s.enabled}};
}

std::optional<core::SatinEffect> satinFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    core::SatinEffect s;
    const auto color = colorField(j, "color");
    std::string blend;
    if (!color || !getStr(j, "blend", blend) || !getF(j, "opacity", s.opacity) ||
        !getF(j, "angle", s.angleDeg) || !getF(j, "distance", s.distance) ||
        !getF(j, "size", s.size) || !getB(j, "invert", s.invert) ||
        !getB(j, "enabled", s.enabled))
        return std::nullopt;
    const auto mode = blendModeFromToken(blend);
    if (!mode)
        return std::nullopt;
    s.color = *color;
    s.blend = *mode;
    return s;
}

json bevelToJson(const core::BevelEffect& b) {
    return json{{"style", tokenOf(kBevelStyleTokens, b.style)}, {"depth", b.depth},
                {"size", b.size}, {"soften", b.soften},
                {"angle", b.angleDeg}, {"altitude", b.altitudeDeg},
                {"highlight", colorToJson(b.highlight)}, {"highlight_opacity", b.highlightOpacity},
                {"shadow", colorToJson(b.shadow)}, {"shadow_opacity", b.shadowOpacity},
                {"enabled", b.enabled}};
}

std::optional<core::BevelEffect> bevelFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    core::BevelEffect b;
    const auto style = enumField(kBevelStyleTokens, j, "style");
    const auto hi = colorField(j, "highlight");
    const auto sh = colorField(j, "shadow");
    if (!style || !hi || !sh || !getF(j, "depth", b.depth) || !getF(j, "size", b.size) ||
        !getF(j, "soften", b.soften) || !getF(j, "angle", b.angleDeg) ||
        !getF(j, "altitude", b.altitudeDeg) || !getF(j, "highlight_opacity", b.highlightOpacity) ||
        !getF(j, "shadow_opacity", b.shadowOpacity) || !getB(j, "enabled", b.enabled))
        return std::nullopt;
    b.style = *style;
    b.highlight = *hi;
    b.shadow = *sh;
    return b;
}

template <typename T, typename ToJson>
json listToJson(const std::vector<T>& v, ToJson&& f) {
    json out = json::array();
    for (const T& item : v)
        out.push_back(f(item));
    return out;
}

template <typename T, typename FromJson>
bool listFromJson(const json& j, const char* k, std::vector<T>& out, FromJson&& f) {
    const json* arr = getArr(j, k);
    if (arr == nullptr)
        return false;
    for (const json& item : *arr) {
        auto v = f(item);
        if (!v)
            return false;
        out.push_back(std::move(*v));
    }
    return true;
}

} // namespace

json effectsToJson(const core::LayerEffects& fx) {
    return json{{"fill_opacity", fx.fillOpacity},
                {"drop_shadows", listToJson(fx.dropShadows, shadowToJson)},
                {"outer_glow", glowToJson(fx.outerGlow)},
                {"color_overlay", overlayToJson(fx.colorOverlay)},
                {"gradient_overlay", overlayToJson(fx.gradientOverlay)},
                {"pattern_overlay", overlayToJson(fx.patternOverlay)},
                {"satin", satinToJson(fx.satin)},
                {"inner_shadows", listToJson(fx.innerShadows, shadowToJson)},
                {"inner_glow", glowToJson(fx.innerGlow)},
                {"bevel", bevelToJson(fx.bevel)},
                {"strokes", listToJson(fx.strokes, strokeEffectToJson)}};
}

std::optional<core::LayerEffects> effectsFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    core::LayerEffects fx;
    const json* outerGlow = getObj(j, "outer_glow");
    const json* innerGlow = getObj(j, "inner_glow");
    const json* colorOv = getObj(j, "color_overlay");
    const json* gradOv = getObj(j, "gradient_overlay");
    const json* patOv = getObj(j, "pattern_overlay");
    const json* satin = getObj(j, "satin");
    const json* bevel = getObj(j, "bevel");
    if (!getF(j, "fill_opacity", fx.fillOpacity) || outerGlow == nullptr ||
        innerGlow == nullptr || colorOv == nullptr || gradOv == nullptr || patOv == nullptr ||
        satin == nullptr || bevel == nullptr)
        return std::nullopt;
    if (!listFromJson(j, "drop_shadows", fx.dropShadows, shadowFromJson) ||
        !listFromJson(j, "inner_shadows", fx.innerShadows, shadowFromJson) ||
        !listFromJson(j, "strokes", fx.strokes, strokeEffectFromJson))
        return std::nullopt;
    auto og = glowFromJson(*outerGlow);
    auto ig = glowFromJson(*innerGlow);
    auto co = overlayFromJson(*colorOv);
    auto go = overlayFromJson(*gradOv);
    auto po = overlayFromJson(*patOv);
    auto sa = satinFromJson(*satin);
    auto be = bevelFromJson(*bevel);
    if (!og || !ig || !co || !go || !po || !sa || !be)
        return std::nullopt;
    fx.outerGlow = std::move(*og);
    fx.innerGlow = std::move(*ig);
    fx.colorOverlay = std::move(*co);
    fx.gradientOverlay = std::move(*go);
    fx.patternOverlay = std::move(*po);
    fx.satin = *sa;
    fx.bevel = *be;
    return fx;
}

// ---- exif (the EXIF READ slice; common/exif.hpp) ------------------------------------------------
// Every field is optional on the wire exactly as in the struct: only present fields are written,
// and the writer never emits an empty node (docio writes "exif" only for hasAny() data), so the
// reader rejects one -- an exif node with nothing in it is not a state the format has. Present-
// but-malformed fields reject the node whole (the file's strict-parse rule), with the SAME value
// contracts the EXIF parser enforces (io/exif.cpp), so nothing a load produced can fail a save.

json exifToJson(const common::ExifData& e) {
    json j = json::object();
    if (e.orientation.has_value())
        j["orientation"] = *e.orientation;
    if (e.focalLengthMm.has_value())
        j["focal_mm"] = *e.focalLengthMm;
    if (e.focalLength35mm.has_value())
        j["focal_35mm"] = *e.focalLength35mm;
    if (e.dateTimeOriginal.has_value())
        j["taken"] = json{{"y", e.dateTimeOriginal->year},   {"mo", e.dateTimeOriginal->month},
                          {"d", e.dateTimeOriginal->day},    {"h", e.dateTimeOriginal->hour},
                          {"mi", e.dateTimeOriginal->minute}, {"s", e.dateTimeOriginal->second}};
    if (e.gpsLatitude.has_value())
        j["lat"] = *e.gpsLatitude;
    if (e.gpsLongitude.has_value())
        j["lon"] = *e.gpsLongitude;
    if (e.make.has_value())
        j["make"] = *e.make;
    if (e.model.has_value())
        j["model"] = *e.model;
    return j;
}

std::optional<common::ExifData> exifFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    common::ExifData e;
    if (j.contains("orientation")) {
        int v = 0;
        if (!getI(j, "orientation", v) || v < 1 || v > 8)
            return std::nullopt;
        e.orientation = v;
    }
    if (j.contains("focal_mm")) {
        double v = 0.0;
        if (!getD(j, "focal_mm", v) || v <= 0.0 || v > 10000.0)
            return std::nullopt;
        e.focalLengthMm = v;
    }
    if (j.contains("focal_35mm")) {
        int v = 0;
        if (!getI(j, "focal_35mm", v) || v < 1 || v > 10000)
            return std::nullopt;
        e.focalLength35mm = v;
    }
    if (j.contains("taken")) {
        const json* t = getObj(j, "taken");
        common::ExifDateTime dt;
        if (t == nullptr || !getI(*t, "y", dt.year) || !getI(*t, "mo", dt.month) ||
            !getI(*t, "d", dt.day) || !getI(*t, "h", dt.hour) || !getI(*t, "mi", dt.minute) ||
            !getI(*t, "s", dt.second) || !common::isValidExifDateTime(dt))
            return std::nullopt;
        e.dateTimeOriginal = dt;
    }
    if (j.contains("lat")) {
        double v = 0.0;
        if (!getD(j, "lat", v) || v < -90.0 || v > 90.0)
            return std::nullopt;
        e.gpsLatitude = v;
    }
    if (j.contains("lon")) {
        double v = 0.0;
        if (!getD(j, "lon", v) || v < -180.0 || v > 180.0)
            return std::nullopt;
        e.gpsLongitude = v;
    }
    if (j.contains("make")) {
        std::string s;
        if (!getStr(j, "make", s) || !common::isValidExifString(s))
            return std::nullopt;
        e.make = std::move(s);
    }
    if (j.contains("model")) {
        std::string s;
        if (!getStr(j, "model", s) || !common::isValidExifString(s))
            return std::nullopt;
        e.model = std::move(s);
    }
    if (!e.hasAny())
        return std::nullopt;  // the writer never emits an empty node (see the section note)
    return e;
}

// ---- texture generator (S55-a; docs/texture-generator.md §3.1) ----------------------------------

namespace {

namespace txg = core::texture;

// The generator tokens mirror the registry (S55-g; texture_render.cpp is the single source of
// truth, its table is constant-initialized so this dynamic mirror can never race it). A new
// generator's token joins docio the moment its registry row lands.
const std::array<std::pair<txg::Generator, const char*>, txg::kGeneratorCount> kGeneratorTokens =
    [] {
        std::array<std::pair<txg::Generator, const char*>, txg::kGeneratorCount> a{};
        for (int i = 0; i < txg::kGeneratorCount; ++i) {
            const auto g = static_cast<txg::Generator>(i);
            a[static_cast<std::size_t>(i)] = {g, txg::generatorTraits(g).token};
        }
        return a;
    }();

constexpr std::array<std::pair<txg::CloudType, const char*>, 10> kCloudTypeTokens{{
    {txg::CloudType::Cirrus, "cirrus"},
    {txg::CloudType::Cirrocumulus, "cirrocumulus"},
    {txg::CloudType::Cirrostratus, "cirrostratus"},
    {txg::CloudType::Altocumulus, "altocumulus"},
    {txg::CloudType::Altostratus, "altostratus"},
    {txg::CloudType::Stratocumulus, "stratocumulus"},
    {txg::CloudType::Stratus, "stratus"},
    {txg::CloudType::Nimbostratus, "nimbostratus"},
    {txg::CloudType::Cumulus, "cumulus"},
    {txg::CloudType::Cumulonimbus, "cumulonimbus"},
}};

constexpr std::array<std::pair<txg::PaperKind, const char*>, 3> kPaperKindTokens{{
    {txg::PaperKind::Wove, "wove"},
    {txg::PaperKind::Laid, "laid"},
    {txg::PaperKind::Felt, "felt"},
}};

json cloudLayerToJson(const txg::CloudLayerParams& l) {
    return json{{"enabled", l.enabled},
                {"type", tokenOf(kCloudTypeTokens, l.type)},
                {"coverage_bias", l.coverageBias},
                {"scale_bias", l.scaleBias},
                {"altitude_m", l.altitudeM}};
}

std::optional<txg::CloudLayerParams> cloudLayerFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    txg::CloudLayerParams l;
    const auto type = enumField(kCloudTypeTokens, j, "type");
    if (!type || !getB(j, "enabled", l.enabled) || !getD(j, "coverage_bias", l.coverageBias) ||
        !getD(j, "scale_bias", l.scaleBias) || !getD(j, "altitude_m", l.altitudeM))
        return std::nullopt;
    l.type = *type;
    return l;
}

// A GROWN field (S55-b onward) reads leniently: absent = the SkyParams default (files written
// before the field predate it -- the §3.1 growth rule), present-but-malformed = reject.
bool getGrownD(const json& j, const char* k, double& out) {
    if (j.find(k) == j.end())
        return true;
    return getD(j, k, out);
}

bool getGrownB(const json& j, const char* k, bool& out) {
    if (j.find(k) == j.end())
        return true;
    return getB(j, k, out);
}

bool getGrownI(const json& j, const char* k, int& out) {
    if (j.find(k) == j.end())
        return true;
    return getI(j, k, out);
}

// The colour twin of getGrownD/getGrownB: absent = keep the default, present-but-malformed = reject.
bool getGrownColor(const json& j, const char* k, common::ColorF& out) {
    if (j.find(k) == j.end())
        return true;
    const auto c = colorField(j, k);
    if (!c)
        return false;
    out = *c;
    return true;
}

// The enum twin of getGrownD/getGrownB: absent = keep the default, present-but-unknown = reject.
template <typename E, std::size_t N>
bool getGrownEnum(const std::array<std::pair<E, const char*>, N>& table, const json& j,
                  const char* k, E& out) {
    if (j.find(k) == j.end())
        return true;
    const auto v = enumField(table, j, k);
    if (!v)
        return false;
    out = *v;
    return true;
}

json skySpecToJson(const txg::SkyParams& s) {
    json layers = json::array();
    for (const auto& l : s.cloudLayers) layers.push_back(cloudLayerToJson(l));
    return json{{"kind", "sky"},
                {"dome", s.enableDome},
                {"sun", s.enableSun},
                {"clouds", s.enableClouds},
                {"haze", s.enableHaze},
                {"sun_azimuth", s.sunAzimuthDeg},
                {"sun_elevation", s.sunElevationDeg},
                {"turbidity", s.turbidity},
                {"cloud_coverage", s.cloudCoverage},
                {"cloud_scale", s.cloudScale},
                {"ground_albedo", s.groundAlbedo},
                {"exposure", s.exposure},
                {"sun_disc_scale", s.sunDiscScale},
                {"fov", s.fovDeg},
                {"pitch", s.pitchDeg},
                {"roll", s.rollDeg},
                {"shift_y", s.shiftY},
                {"wind_direction", s.windDirectionDeg},
                {"wind_strength", s.windStrength},
                {"volumetric_clouds", s.volumetricClouds},
                {"moon", s.enableMoon},
                {"moon_azimuth", s.moonAzimuthDeg},
                {"moon_elevation", s.moonElevationDeg},
                {"moon_scale", s.moonScale},
                {"moon_phase_mode", s.moonPhaseMode},
                {"moon_illuminated_fraction", s.moonIlluminatedFraction},
                {"stars_amount", s.starsAmount},
                {"obs_year", s.obsYear},
                {"obs_month", s.obsMonth},
                {"obs_day", s.obsDay},
                {"obs_hour_utc", s.obsHourUtc},
                {"obs_latitude", s.obsLatitudeDeg},
                {"obs_longitude", s.obsLongitudeDeg},
                {"lens_flare", s.enableLensFlare},
                {"flare_strength", s.flareStrength},
                {"cloud_layers", std::move(layers)}};
}

std::optional<txg::SkyParams> skySpecFromJson(const json& j) {
    txg::SkyParams s;
    // The S55-a nine are strict (every sky spec ever written carries them)...
    if (!getB(j, "dome", s.enableDome) || !getB(j, "sun", s.enableSun) ||
        !getB(j, "clouds", s.enableClouds) || !getB(j, "haze", s.enableHaze) ||
        !getD(j, "sun_azimuth", s.sunAzimuthDeg) ||
        !getD(j, "sun_elevation", s.sunElevationDeg) || !getD(j, "turbidity", s.turbidity) ||
        !getD(j, "cloud_coverage", s.cloudCoverage) || !getD(j, "cloud_scale", s.cloudScale))
        return std::nullopt;
    // ...the S55-b growth reads leniently so pre-growth files keep loading (schema stays 1).
    if (!getGrownD(j, "ground_albedo", s.groundAlbedo) || !getGrownD(j, "exposure", s.exposure) ||
        !getGrownD(j, "sun_disc_scale", s.sunDiscScale) || !getGrownD(j, "fov", s.fovDeg) ||
        !getGrownD(j, "pitch", s.pitchDeg) || !getGrownD(j, "roll", s.rollDeg) ||
        !getGrownD(j, "shift_y", s.shiftY) ||
        !getGrownD(j, "wind_direction", s.windDirectionDeg) ||
        !getGrownD(j, "wind_strength", s.windStrength) ||
        !getGrownB(j, "volumetric_clouds", s.volumetricClouds) ||
        // -- S55-f night growth --
        !getGrownB(j, "moon", s.enableMoon) || !getGrownD(j, "moon_azimuth", s.moonAzimuthDeg) ||
        !getGrownD(j, "moon_elevation", s.moonElevationDeg) ||
        !getGrownD(j, "moon_scale", s.moonScale) ||
        !getGrownI(j, "moon_phase_mode", s.moonPhaseMode) ||
        !getGrownD(j, "moon_illuminated_fraction", s.moonIlluminatedFraction) ||
        !getGrownD(j, "stars_amount", s.starsAmount) ||
        // -- S55 night-overhaul observer clock --
        !getGrownI(j, "obs_year", s.obsYear) || !getGrownI(j, "obs_month", s.obsMonth) ||
        !getGrownI(j, "obs_day", s.obsDay) || !getGrownD(j, "obs_hour_utc", s.obsHourUtc) ||
        !getGrownD(j, "obs_latitude", s.obsLatitudeDeg) ||
        !getGrownD(j, "obs_longitude", s.obsLongitudeDeg) ||
        // -- lens-flare growth --
        !getGrownB(j, "lens_flare", s.enableLensFlare) ||
        !getGrownD(j, "flare_strength", s.flareStrength))
        return std::nullopt;
    if (const json* layers = getArr(j, "cloud_layers")) {
        s.cloudLayers.clear();
        for (const json& lj : *layers) {
            auto l = cloudLayerFromJson(lj);
            if (!l)
                return std::nullopt;
            s.cloudLayers.push_back(*l);
        }
    }
    return s;
}

json paperSpecToJson(const txg::PaperParams& p) {
    return json{{"kind", "paper"},
                {"tint", colorToJson(p.tint)},
                {"roughness", p.roughness},
                {"grain_angle", p.grainAngleDeg},
                {"grain_anisotropy", p.grainAnisotropy},
                {"light_azimuth", p.lightAzimuthDeg},
                {"light_elevation", p.lightElevationDeg},
                // -- S55-d growth --
                {"paper_kind", tokenOf(kPaperKindTokens, p.kind)},
                {"fiber", p.fiber},
                {"laid_spacing", p.laidSpacing},
                {"chain_spacing", p.chainSpacing},
                {"laid_depth", p.laidDepth},
                {"matte", p.matte},
                {"sheen", p.sheen},
                {"deckle_edge", p.deckleEdge},
                {"deckle_amount", p.deckleAmount},
                {"deckle_inset", p.deckleInset},
                {"print_tooth", p.printTooth},
                {"print_amount", p.printAmount}};
}

std::optional<txg::PaperParams> paperSpecFromJson(const json& j) {
    txg::PaperParams p;
    // The S55-a six are strict (every paper spec ever written carries them)...
    const auto tint = colorField(j, "tint");
    if (!tint || !getD(j, "roughness", p.roughness) ||
        !getD(j, "grain_angle", p.grainAngleDeg) ||
        !getD(j, "grain_anisotropy", p.grainAnisotropy) ||
        !getD(j, "light_azimuth", p.lightAzimuthDeg) ||
        !getD(j, "light_elevation", p.lightElevationDeg))
        return std::nullopt;
    // ...the S55-d growth reads leniently so pre-growth files keep loading (schema stays 1).
    if (!getGrownEnum(kPaperKindTokens, j, "paper_kind", p.kind) ||
        !getGrownD(j, "fiber", p.fiber) || !getGrownD(j, "laid_spacing", p.laidSpacing) ||
        !getGrownD(j, "chain_spacing", p.chainSpacing) ||
        !getGrownD(j, "laid_depth", p.laidDepth) || !getGrownD(j, "matte", p.matte) ||
        !getGrownD(j, "sheen", p.sheen) || !getGrownB(j, "deckle_edge", p.deckleEdge) ||
        !getGrownD(j, "deckle_amount", p.deckleAmount) ||
        !getGrownD(j, "deckle_inset", p.deckleInset) ||
        !getGrownB(j, "print_tooth", p.printTooth) ||
        !getGrownD(j, "print_amount", p.printAmount))
        return std::nullopt;
    p.tint = *tint;
    return p;
}

json grassSpecToJson(const txg::GrassParams& g) {
    return json{{"kind", "grass"},
                {"base_color", colorToJson(g.baseColor)},
                {"tip_color", colorToJson(g.tipColor)},
                {"clump_scale", g.clumpScale},
                {"patchiness", g.patchiness},
                // -- S55-e growth --
                {"soil_color", colorToJson(g.soilColor)},
                {"dry_color", colorToJson(g.dryColor)},
                {"enable_turf", g.enableTurf},
                {"enable_blades", g.enableBlades},
                {"density", g.density},
                {"blade_height", g.bladeHeight},
                {"blade_width", g.bladeWidth},
                {"curvature", g.curvature},
                {"wind_direction", g.windDirectionDeg},
                {"wind_strength", g.windStrength},
                {"fov", g.fovDeg},
                {"pitch", g.pitchDeg},
                {"light_azimuth", g.lightAzimuthDeg},
                {"light_elevation", g.lightElevationDeg},
                {"dry_amount", g.dryAmount}};
}

std::optional<txg::GrassParams> grassSpecFromJson(const json& j) {
    txg::GrassParams g;
    // The S55-a four are strict (every grass spec ever written carries them)...
    const auto base = colorField(j, "base_color");
    const auto tip = colorField(j, "tip_color");
    if (!base || !tip || !getD(j, "clump_scale", g.clumpScale) ||
        !getD(j, "patchiness", g.patchiness))
        return std::nullopt;
    g.baseColor = *base;
    g.tipColor = *tip;
    // ...the S55-e growth reads leniently so pre-growth files keep loading (schema stays 1).
    if (!getGrownColor(j, "soil_color", g.soilColor) ||
        !getGrownColor(j, "dry_color", g.dryColor) ||
        !getGrownB(j, "enable_turf", g.enableTurf) ||
        !getGrownB(j, "enable_blades", g.enableBlades) || !getGrownD(j, "density", g.density) ||
        !getGrownD(j, "blade_height", g.bladeHeight) ||
        !getGrownD(j, "blade_width", g.bladeWidth) || !getGrownD(j, "curvature", g.curvature) ||
        !getGrownD(j, "wind_direction", g.windDirectionDeg) ||
        !getGrownD(j, "wind_strength", g.windStrength) || !getGrownD(j, "fov", g.fovDeg) ||
        !getGrownD(j, "pitch", g.pitchDeg) ||
        !getGrownD(j, "light_azimuth", g.lightAzimuthDeg) ||
        !getGrownD(j, "light_elevation", g.lightElevationDeg) ||
        !getGrownD(j, "dry_amount", g.dryAmount))
        return std::nullopt;
    return g;
}

// ---- the S55-g material arms. Born whole, so every field reads STRICT (the "S55-a fields" rule
// for a new arm: everything the writer has ever emitted is required; future growth fields will
// read leniently through getGrown*). Kind tags are the registry tokens.

json woodSpecToJson(const txg::WoodParams& v) {
    return json{{"kind", "wood"},
                {"early_color", colorToJson(v.earlyColor)},
                {"late_color", colorToJson(v.lateColor)},
                {"ring_spacing", v.ringSpacing},
                {"ring_contrast", v.ringContrast},
                {"waviness", v.waviness},
                {"knots", v.knots},
                {"fiber", v.fiber},
                {"grain_angle", v.grainAngleDeg},
                {"roughness", v.roughness},
                {"matte", v.matte},
                {"sheen", v.sheen},
                {"light_azimuth", v.lightAzimuthDeg},
                {"light_elevation", v.lightElevationDeg}};
}

std::optional<txg::WoodParams> woodSpecFromJson(const json& j) {
    txg::WoodParams v;
    const auto early = colorField(j, "early_color");
    const auto late = colorField(j, "late_color");
    if (!early || !late || !getD(j, "ring_spacing", v.ringSpacing) ||
        !getD(j, "ring_contrast", v.ringContrast) || !getD(j, "waviness", v.waviness) ||
        !getD(j, "knots", v.knots) || !getD(j, "fiber", v.fiber) ||
        !getD(j, "grain_angle", v.grainAngleDeg) || !getD(j, "roughness", v.roughness) ||
        !getD(j, "matte", v.matte) || !getD(j, "sheen", v.sheen) ||
        !getD(j, "light_azimuth", v.lightAzimuthDeg) ||
        !getD(j, "light_elevation", v.lightElevationDeg))
        return std::nullopt;
    v.earlyColor = *early;
    v.lateColor = *late;
    return v;
}

json marbleSpecToJson(const txg::MarbleParams& v) {
    return json{{"kind", "marble"},
                {"base_color", colorToJson(v.baseColor)},
                {"vein_color", colorToJson(v.veinColor)},
                {"vein_spacing", v.veinSpacing},
                {"turbulence", v.turbulence},
                {"contrast", v.contrast},
                {"vein_angle", v.veinAngleDeg},
                {"roughness", v.roughness},
                {"matte", v.matte},
                {"sheen", v.sheen},
                {"light_azimuth", v.lightAzimuthDeg},
                {"light_elevation", v.lightElevationDeg}};
}

std::optional<txg::MarbleParams> marbleSpecFromJson(const json& j) {
    txg::MarbleParams v;
    const auto base = colorField(j, "base_color");
    const auto vein = colorField(j, "vein_color");
    if (!base || !vein || !getD(j, "vein_spacing", v.veinSpacing) ||
        !getD(j, "turbulence", v.turbulence) || !getD(j, "contrast", v.contrast) ||
        !getD(j, "vein_angle", v.veinAngleDeg) || !getD(j, "roughness", v.roughness) ||
        !getD(j, "matte", v.matte) || !getD(j, "sheen", v.sheen) ||
        !getD(j, "light_azimuth", v.lightAzimuthDeg) ||
        !getD(j, "light_elevation", v.lightElevationDeg))
        return std::nullopt;
    v.baseColor = *base;
    v.veinColor = *vein;
    return v;
}

json stoneSpecToJson(const txg::StoneParams& v) {
    return json{{"kind", "stone"},
                {"base_color", colorToJson(v.baseColor)},
                {"cell_size", v.cellSize},
                {"crack_depth", v.crackDepth},
                {"roughness", v.roughness},
                {"variation", v.variation},
                {"matte", v.matte},
                {"sheen", v.sheen},
                {"light_azimuth", v.lightAzimuthDeg},
                {"light_elevation", v.lightElevationDeg}};
}

std::optional<txg::StoneParams> stoneSpecFromJson(const json& j) {
    txg::StoneParams v;
    const auto base = colorField(j, "base_color");
    if (!base || !getD(j, "cell_size", v.cellSize) || !getD(j, "crack_depth", v.crackDepth) ||
        !getD(j, "roughness", v.roughness) || !getD(j, "variation", v.variation) ||
        !getD(j, "matte", v.matte) || !getD(j, "sheen", v.sheen) ||
        !getD(j, "light_azimuth", v.lightAzimuthDeg) ||
        !getD(j, "light_elevation", v.lightElevationDeg))
        return std::nullopt;
    v.baseColor = *base;
    return v;
}

json canvasSpecToJson(const txg::CanvasParams& v) {
    return json{{"kind", "canvas"},
                {"tint", colorToJson(v.tint)},
                {"thread_pitch", v.threadPitch},
                {"irregularity", v.irregularity},
                {"weave_depth", v.weaveDepth},
                {"weave_angle", v.weaveAngleDeg},
                {"fuzz", v.fuzz},
                {"matte", v.matte},
                {"sheen", v.sheen},
                {"light_azimuth", v.lightAzimuthDeg},
                {"light_elevation", v.lightElevationDeg}};
}

std::optional<txg::CanvasParams> canvasSpecFromJson(const json& j) {
    txg::CanvasParams v;
    const auto tint = colorField(j, "tint");
    if (!tint || !getD(j, "thread_pitch", v.threadPitch) ||
        !getD(j, "irregularity", v.irregularity) || !getD(j, "weave_depth", v.weaveDepth) ||
        !getD(j, "weave_angle", v.weaveAngleDeg) || !getD(j, "fuzz", v.fuzz) ||
        !getD(j, "matte", v.matte) || !getD(j, "sheen", v.sheen) ||
        !getD(j, "light_azimuth", v.lightAzimuthDeg) ||
        !getD(j, "light_elevation", v.lightElevationDeg))
        return std::nullopt;
    v.tint = *tint;
    return v;
}

json metalSpecToJson(const txg::MetalParams& v) {
    return json{{"kind", "metal"},
                {"tint", colorToJson(v.tint)},
                {"brush_angle", v.brushAngleDeg},
                {"roughness", v.roughness},
                {"sheen", v.sheen},
                {"gradient", v.gradient},
                {"matte", v.matte},
                {"light_azimuth", v.lightAzimuthDeg},
                {"light_elevation", v.lightElevationDeg}};
}

std::optional<txg::MetalParams> metalSpecFromJson(const json& j) {
    txg::MetalParams v;
    const auto tint = colorField(j, "tint");
    if (!tint || !getD(j, "brush_angle", v.brushAngleDeg) ||
        !getD(j, "roughness", v.roughness) || !getD(j, "sheen", v.sheen) ||
        !getD(j, "gradient", v.gradient) || !getD(j, "matte", v.matte) ||
        !getD(j, "light_azimuth", v.lightAzimuthDeg) ||
        !getD(j, "light_elevation", v.lightElevationDeg))
        return std::nullopt;
    v.tint = *tint;
    return v;
}

}  // namespace

json textureParamsToJson(const core::texture::TextureParams& p) {
    // The spec node carries its OWN arm tag: generator and spec round-trip independently, so
    // even a (never-constructed-in-practice) mismatched value survives save/load unchanged.
    json spec;
    if (const auto* s = std::get_if<txg::SkyParams>(&p.spec))
        spec = skySpecToJson(*s);
    else if (const auto* pp = std::get_if<txg::PaperParams>(&p.spec))
        spec = paperSpecToJson(*pp);
    else if (const auto* g = std::get_if<txg::GrassParams>(&p.spec))
        spec = grassSpecToJson(*g);
    else if (const auto* wo = std::get_if<txg::WoodParams>(&p.spec))
        spec = woodSpecToJson(*wo);
    else if (const auto* ma = std::get_if<txg::MarbleParams>(&p.spec))
        spec = marbleSpecToJson(*ma);
    else if (const auto* st = std::get_if<txg::StoneParams>(&p.spec))
        spec = stoneSpecToJson(*st);
    else if (const auto* cv = std::get_if<txg::CanvasParams>(&p.spec))
        spec = canvasSpecToJson(*cv);
    else if (const auto* me = std::get_if<txg::MetalParams>(&p.spec))
        spec = metalSpecToJson(*me);
    return json{{"generator", tokenOf(kGeneratorTokens, p.generator)},
                {"seed", p.seed},
                {"scale", p.scale},
                {"spec", std::move(spec)}};
}

std::optional<core::texture::TextureParams> textureParamsFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    txg::TextureParams p;
    const auto gen = enumField(kGeneratorTokens, j, "generator");
    const json* spec = getObj(j, "spec");
    if (!gen || spec == nullptr || !getU64(j, "seed", p.seed) || !getD(j, "scale", p.scale))
        return std::nullopt;
    p.generator = *gen;
    const auto arm = enumField(kGeneratorTokens, *spec, "kind");
    if (!arm)
        return std::nullopt;
    switch (*arm) {
        case txg::Generator::Sky: {
            const auto s = skySpecFromJson(*spec);
            if (!s)
                return std::nullopt;
            p.spec = *s;
            break;
        }
        case txg::Generator::Paper: {
            const auto pp = paperSpecFromJson(*spec);
            if (!pp)
                return std::nullopt;
            p.spec = *pp;
            break;
        }
        case txg::Generator::Grass: {
            const auto g = grassSpecFromJson(*spec);
            if (!g)
                return std::nullopt;
            p.spec = *g;
            break;
        }
        case txg::Generator::Wood: {
            const auto v = woodSpecFromJson(*spec);
            if (!v)
                return std::nullopt;
            p.spec = *v;
            break;
        }
        case txg::Generator::Marble: {
            const auto v = marbleSpecFromJson(*spec);
            if (!v)
                return std::nullopt;
            p.spec = *v;
            break;
        }
        case txg::Generator::Stone: {
            const auto v = stoneSpecFromJson(*spec);
            if (!v)
                return std::nullopt;
            p.spec = *v;
            break;
        }
        case txg::Generator::Canvas: {
            const auto v = canvasSpecFromJson(*spec);
            if (!v)
                return std::nullopt;
            p.spec = *v;
            break;
        }
        case txg::Generator::Metal: {
            const auto v = metalSpecFromJson(*spec);
            if (!v)
                return std::nullopt;
            p.spec = *v;
            break;
        }
    }
    return p;
}

// ---- text --------------------------------------------------------------------------------------

namespace {

json fontToJson(const tx::FontRef& f) {
    return json{{"family", f.family}, {"weight", f.weight}, {"italic", f.italic},
                {"width_axis", f.widthAxis}, {"variations", f.variations}};
}

std::optional<tx::FontRef> fontFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    tx::FontRef f;
    const json* vars = getObj(j, "variations");
    if (vars == nullptr || !getStr(j, "family", f.family) || !getF(j, "weight", f.weight) ||
        !getB(j, "italic", f.italic) || !getF(j, "width_axis", f.widthAxis))
        return std::nullopt;
    for (const auto& [key, value] : vars->items()) {
        if (!value.is_number())
            return std::nullopt;
        f.variations[key] = value.get<float>();
    }
    return f;
}

json charStyleToJson(const tx::CharStyle& s) {
    return json{{"font", fontToJson(s.font)}, {"size", s.sizePx},
                {"paint", paintToJson(s.paint)}, {"underline", s.underline},
                {"strike", s.strikethrough}, {"tracking", s.tracking},
                {"baseline_shift", s.baselineShift}, {"features", s.features},
                {"kerning", tokenOf(kKerningTokens, s.kerning)}};
}

std::optional<tx::CharStyle> charStyleFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    tx::CharStyle s;
    const json* font = getObj(j, "font");
    auto paint = paintField(j, "paint");
    const auto kerning = enumField(kKerningTokens, j, "kerning");
    const json* features = getArr(j, "features");
    if (font == nullptr || !paint || !kerning || features == nullptr ||
        !getF(j, "size", s.sizePx) || !getB(j, "underline", s.underline) ||
        !getB(j, "strike", s.strikethrough) || !getF(j, "tracking", s.tracking) ||
        !getF(j, "baseline_shift", s.baselineShift))
        return std::nullopt;
    auto f = fontFromJson(*font);
    if (!f)
        return std::nullopt;
    s.font = std::move(*f);
    s.paint = std::move(*paint);
    s.kerning = *kerning;
    for (const json& feat : *features) {
        if (!feat.is_string())
            return std::nullopt;
        s.features.push_back(feat.get<std::string>());
    }
    return s;
}

json paragraphToJson(const tx::Paragraph& p) {
    return json{{"align", tokenOf(kParaAlignTokens, p.align)}, {"leading", p.leading},
                {"leading_absolute", p.leadingAbsolute}, {"space_before", p.spaceBefore},
                {"space_after", p.spaceAfter}, {"indent_first", p.indentFirst},
                {"indent_left", p.indentLeft}, {"indent_right", p.indentRight},
                {"direction", tokenOf(kParaDirTokens, p.direction)}, {"language", p.language},
                {"hyphenate", p.hyphenate}};
}

std::optional<tx::Paragraph> paragraphFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    tx::Paragraph p;
    const auto align = enumField(kParaAlignTokens, j, "align");
    const auto dir = enumField(kParaDirTokens, j, "direction");
    if (!align || !dir || !getF(j, "leading", p.leading) ||
        !getB(j, "leading_absolute", p.leadingAbsolute) ||
        !getF(j, "space_before", p.spaceBefore) || !getF(j, "space_after", p.spaceAfter) ||
        !getF(j, "indent_first", p.indentFirst) || !getF(j, "indent_left", p.indentLeft) ||
        !getF(j, "indent_right", p.indentRight) || !getStr(j, "language", p.language) ||
        !getB(j, "hyphenate", p.hyphenate))
        return std::nullopt;
    p.align = *align;
    p.direction = *dir;
    return p;
}

json contoursToJson(const vc::Contours& cs) {
    json out = json::array();
    for (const vc::Contour& c : cs) {
        json pts = json::array();
        for (const common::Vec2 p : c.points) {
            pts.push_back(p.x);
            pts.push_back(p.y);
        }
        out.push_back(json{{"closed", c.closed}, {"points", std::move(pts)}});
    }
    return out;
}

std::optional<vc::Contours> contoursFromJson(const json& j) {
    if (!j.is_array())
        return std::nullopt;
    vc::Contours out;
    for (const json& cj : j) {
        if (!cj.is_object())
            return std::nullopt;
        vc::Contour c;
        const json* pts = getArr(cj, "points");
        if (pts == nullptr || pts->size() % 2 != 0 || !getB(cj, "closed", c.closed))
            return std::nullopt;
        for (std::size_t i = 0; i < pts->size(); i += 2) {
            if (!(*pts)[i].is_number() || !(*pts)[i + 1].is_number())
                return std::nullopt;
            c.points.push_back({(*pts)[i].get<double>(), (*pts)[i + 1].get<double>()});
        }
        out.push_back(std::move(c));
    }
    return out;
}

json materialToJson(const tx::Material& m) {
    return json{{"albedo", colorToJson(m.albedo)}, {"metalness", m.metalness},
                {"roughness", m.roughness}};
}

std::optional<tx::Material> materialFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    tx::Material m;
    const auto albedo = colorField(j, "albedo");
    if (!albedo || !getF(j, "metalness", m.metalness) || !getF(j, "roughness", m.roughness))
        return std::nullopt;
    m.albedo = *albedo;
    return m;
}

json extrudeBevelToJson(const tx::Bevel& b) {
    return json{{"profile", tokenOf(kBevelProfileTokens, b.profile)}, {"size", b.size},
                {"segments", b.segments}};
}

std::optional<tx::Bevel> extrudeBevelFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    tx::Bevel b;
    const auto profile = enumField(kBevelProfileTokens, j, "profile");
    if (!profile || !getF(j, "size", b.size) || !getI(j, "segments", b.segments))
        return std::nullopt;
    b.profile = *profile;
    return b;
}

json extrudeToJson(const tx::Extrude& e) {
    json lights = json::array();
    for (const tx::Light& l : e.lights)
        lights.push_back(json{{"direction", json::array({l.direction.x, l.direction.y,
                                                         l.direction.z})},
                              {"color", colorToJson(l.color)},
                              {"intensity", l.intensity}});
    json runMaterials = json::object();
    for (const auto& [run, mat] : e.runMaterials)
        runMaterials[std::to_string(run)] = materialToJson(mat);
    return json{{"depth", e.depth},
                {"bevel_front", extrudeBevelToJson(e.bevelFront)},
                {"bevel_back", extrudeBevelToJson(e.bevelBack)},
                {"material", materialToJson(e.material)},
                {"orientation", json::array({e.orientation.w, e.orientation.x, e.orientation.y,
                                             e.orientation.z})},
                {"perspective", e.perspective},
                {"lighting", e.lightingEnabled},
                {"lights", std::move(lights)},
                {"ambient", colorToJson(e.ambient)},
                {"reflect_canvas", e.reflectCanvas},
                {"reflect_sides_only", e.reflectSidesOnly},
                {"run_materials", std::move(runMaterials)}};
}

std::optional<tx::Extrude> extrudeFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    tx::Extrude e;
    const json* front = getObj(j, "bevel_front");
    const json* back = getObj(j, "bevel_back");
    const json* mat = getObj(j, "material");
    const json* orient = getArr(j, "orientation");
    const json* lights = getArr(j, "lights");
    const json* runMats = getObj(j, "run_materials");
    const auto ambient = colorField(j, "ambient");
    if (front == nullptr || back == nullptr || mat == nullptr || orient == nullptr ||
        orient->size() != 4 || lights == nullptr || runMats == nullptr || !ambient ||
        !getF(j, "depth", e.depth) || !getF(j, "perspective", e.perspective) ||
        !getB(j, "lighting", e.lightingEnabled) || !getB(j, "reflect_canvas", e.reflectCanvas) ||
        !getB(j, "reflect_sides_only", e.reflectSidesOnly))
        return std::nullopt;
    auto bf = extrudeBevelFromJson(*front);
    auto bb = extrudeBevelFromJson(*back);
    auto m = materialFromJson(*mat);
    if (!bf || !bb || !m)
        return std::nullopt;
    e.bevelFront = *bf;
    e.bevelBack = *bb;
    e.material = *m;
    e.ambient = *ambient;
    for (std::size_t i = 0; i < 4; ++i)
        if (!(*orient)[i].is_number())
            return std::nullopt;
    e.orientation = {(*orient)[0].get<double>(), (*orient)[1].get<double>(),
                     (*orient)[2].get<double>(), (*orient)[3].get<double>()};
    e.lights.clear();
    for (const json& lj : *lights) {
        if (!lj.is_object())
            return std::nullopt;
        tx::Light l;
        const json* dir = getArr(lj, "direction");
        const auto color = colorField(lj, "color");
        if (dir == nullptr || dir->size() != 3 || !color || !getF(lj, "intensity", l.intensity))
            return std::nullopt;
        for (std::size_t i = 0; i < 3; ++i)
            if (!(*dir)[i].is_number())
                return std::nullopt;
        l.direction = {(*dir)[0].get<double>(), (*dir)[1].get<double>(), (*dir)[2].get<double>()};
        l.color = *color;
        e.lights.push_back(l);
    }
    for (const auto& [key, value] : runMats->items()) {
        char* end = nullptr;
        const unsigned long long run = std::strtoull(key.c_str(), &end, 10);
        if (end == nullptr || *end != '\0')
            return std::nullopt;
        auto rm = materialFromJson(value);
        if (!rm)
            return std::nullopt;
        e.runMaterials[static_cast<std::size_t>(run)] = *rm;
    }
    return e;
}

} // namespace

json textBlockToJson(const tx::TextBlock& b) {
    json runs = json::array();
    for (const tx::StyleRun& r : b.runs)
        runs.push_back(json{{"begin", r.begin}, {"end", r.end},
                            {"style", charStyleToJson(r.style)}});
    json paragraphs = json::array();
    for (const tx::Paragraph& p : b.paragraphs)
        paragraphs.push_back(paragraphToJson(p));
    json out{{"utf8", b.utf8},
             {"runs", std::move(runs)},
             {"paragraphs", std::move(paragraphs)},
             {"frame", tokenOf(kFrameTokens, b.frame)},
             {"area_size", vec2ToJson(b.areaSize)},
             {"aa", tokenOf(kAaTokens, b.aa)},
             {"writing_mode", tokenOf(kWritingModeTokens, b.writingMode)},
             {"orientation", tokenOf(kOrientationTokens, b.orientation)},
             {"bend", b.bend},
             {"empty_style", charStyleToJson(b.emptyStyle)}};
    if (b.pathFit.has_value()) {
        out["path_fit"] = json{{"layer", b.pathFit->layer}, {"s0", b.pathFit->s0},
                               {"s1", b.pathFit->s1}, {"flip", b.pathFit->flip},
                               {"baked", contoursToJson(b.pathFit->baked)}};
    }
    if (b.extrude.has_value())
        out["extrude"] = extrudeToJson(*b.extrude);
    return out;
}

std::optional<tx::TextBlock> textBlockFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    tx::TextBlock b;
    const json* runs = getArr(j, "runs");
    const json* paragraphs = getArr(j, "paragraphs");
    const json* emptyStyle = getObj(j, "empty_style");
    const auto frame = enumField(kFrameTokens, j, "frame");
    const auto aa = enumField(kAaTokens, j, "aa");
    const auto wm = enumField(kWritingModeTokens, j, "writing_mode");
    const auto orient = enumField(kOrientationTokens, j, "orientation");
    const auto area = j.contains("area_size") ? vec2FromJson(j["area_size"])
                                              : std::optional<common::Vec2>{};
    if (runs == nullptr || paragraphs == nullptr || emptyStyle == nullptr || !frame || !aa ||
        !wm || !orient || !area || !getStr(j, "utf8", b.utf8) || !getF(j, "bend", b.bend))
        return std::nullopt;
    b.frame = *frame;
    b.aa = *aa;
    b.writingMode = *wm;
    b.orientation = *orient;
    b.areaSize = *area;
    auto es = charStyleFromJson(*emptyStyle);
    if (!es)
        return std::nullopt;
    b.emptyStyle = std::move(*es);
    for (const json& rj : *runs) {
        if (!rj.is_object())
            return std::nullopt;
        tx::StyleRun r;
        const json* style = getObj(rj, "style");
        std::uint64_t begin = 0, end = 0;
        if (style == nullptr || !getU64(rj, "begin", begin) || !getU64(rj, "end", end))
            return std::nullopt;
        r.begin = static_cast<std::size_t>(begin);
        r.end = static_cast<std::size_t>(end);
        auto s = charStyleFromJson(*style);
        if (!s)
            return std::nullopt;
        r.style = std::move(*s);
        b.runs.push_back(std::move(r));
    }
    for (const json& pj : *paragraphs) {
        auto p = paragraphFromJson(pj);
        if (!p)
            return std::nullopt;
        b.paragraphs.push_back(std::move(*p));
    }
    if (const json* pf = getObj(j, "path_fit"); pf != nullptr) {
        tx::PathFit fit;
        const json* baked = getArr(*pf, "baked");
        if (baked == nullptr || !getU64(*pf, "layer", fit.layer) || !getD(*pf, "s0", fit.s0) ||
            !getD(*pf, "s1", fit.s1) || !getB(*pf, "flip", fit.flip))
            return std::nullopt;
        auto contours = contoursFromJson(*baked);
        if (!contours)
            return std::nullopt;
        fit.baked = std::move(*contours);
        b.pathFit = std::move(fit);
    }
    if (const json* ex = getObj(j, "extrude"); ex != nullptr) {
        auto e = extrudeFromJson(*ex);
        if (!e)
            return std::nullopt;
        b.extrude = std::move(*e);
    }
    // The run-list invariant is the model's most error-prone bit; a file from a buggy or hostile
    // writer must not smuggle an invalid block into the session. Normalize forces the invariant
    // (idempotent for well-formed input, so honest round-trips are untouched).
    tx::normalize(b);
    return b;
}

// ---- warp (S35-b; docs/warp-tools.md §7) --------------------------------------------------------
// An additive, optional per-layer node. The manifest SCHEMA VERSION STAYS 1 -- adding a node an
// older reader ignores changes nothing about how it reads the rest -- and the reader is STRICT:
// absent is fine, present-but-malformed refuses the file. That is the established rule for the
// per-layer "exif" node and this node copies its shape exactly, down to never emitting a node the
// reader would reject (docio writes "warp" only for a valid grid).
//
// Every number is layer-local px in the layer's OWN pixel space (core::WarpGrid's contract), so a
// document that reloads gets its handles back exactly where they were, with no transform to compose.

namespace {

constexpr std::array<std::pair<core::WarpKind, const char*>, 2> kWarpKindTokens{{
    {core::WarpKind::Mesh, "mesh"},
    {core::WarpKind::Perspective, "perspective"},
}};

} // namespace

json warpToJson(const core::WarpGrid& g) {
    json pts = json::array();
    for (const common::Vec2& p : g.points)
        pts.push_back(vec2ToJson(p));
    return json{{"kind", tokenOf(kWarpKindTokens, g.kind)},
                {"cols", g.cols},
                {"rows", g.rows},
                {"src", json::array({g.source.x, g.source.y, g.source.w, g.source.h})},
                {"pts", std::move(pts)}};
}

std::optional<core::WarpGrid> warpFromJson(const json& j) {
    if (!j.is_object())
        return std::nullopt;
    std::string kind;
    if (!getStr(j, "kind", kind))
        return std::nullopt;
    const auto k = enumOf(kWarpKindTokens, kind);
    if (!k)
        return std::nullopt;
    core::WarpGrid g;
    g.kind = *k;
    if (!getI(j, "cols", g.cols) || !getI(j, "rows", g.rows))
        return std::nullopt;
    const json* src = getArr(j, "src");
    if (src == nullptr || src->size() != 4)
        return std::nullopt;
    for (const json& v : *src)
        if (!v.is_number())
            return std::nullopt;
    g.source = common::Rect{(*src)[0].get<double>(), (*src)[1].get<double>(),
                            (*src)[2].get<double>(), (*src)[3].get<double>()};
    const json* pts = getArr(j, "pts");
    if (pts == nullptr)
        return std::nullopt;
    g.points.reserve(pts->size());
    for (const json& p : *pts) {
        const auto v = vec2FromJson(p);
        if (!v)
            return std::nullopt;
        g.points.push_back(*v);
    }
    // valid() is the whole of the contract: cols/rows >= 2, exactly cols*rows points, a
    // non-degenerate source rect, and 2x2 for Perspective. A node that fails it describes a lattice
    // the kernel and the overlay could not agree about, so it is a malformed node, not a
    // best-effort one -- the format's strict-parse rule.
    if (!g.valid())
        return std::nullopt;
    // The one bound the model itself does not carry: the tool's own lattice ceiling
    // (ui::kWarpMaxNodes). Restated as a literal rather than reached for, because io must not depend
    // on ui -- if the tool's cap ever moves, this is the second place to move it, and a mismatch
    // costs only a refused (absurd) file.
    if (g.cols > 12 || g.rows > 12)
        return std::nullopt;
    return g;
}

} // namespace mosaic::io::native::detail
