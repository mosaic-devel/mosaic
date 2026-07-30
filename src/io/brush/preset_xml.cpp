#include "io/brush/preset_xml.hpp"

#include "core/brush/curve.hpp"
#include "core/brush/parse_util.hpp"
#include "core/brush/xml_util.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <limits>

namespace mosaic::io::brush {
namespace {

namespace cb = mosaic::core::brush;
using cb::detail::boolAttribute;
using cb::detail::doubleAttribute;
using cb::detail::intAttribute;

// pugixml materializes the whole document several times over before any per-element cap can run,
// so the byte count is the cap that actually bounds memory (the sensors.cpp lesson). The largest
// shipped preset XML is ~60 KB; a v5 preset embedding resources can be far bigger, so this tracks
// the walker's total-text budget rather than the sensor fragments' 64 KB.
constexpr std::size_t kMaxDocumentBytes = kMaxPresetTotalBytes;

[[nodiscard]] bool loadDocument(pugi::xml_document& doc, std::string_view xml,
                                std::string* error) {
    if (xml.size() > kMaxDocumentBytes) {
        if (error != nullptr)
            *error = "the XML document is implausibly large";
        return false;
    }
    // parse_default handles the shipped shapes (no preset document opens with a DOCTYPE, but the
    // flag costs nothing and a third-party writer might); CDATA becomes plain child text.
    const pugi::xml_parse_result ok =
        doc.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_doctype);
    if (!ok) {
        if (error != nullptr)
            *error = std::string("XML parse error: ") + ok.description();
        return false;
    }
    return true;
}

[[nodiscard]] PresetParam::Type paramType(std::string_view t) noexcept {
    if (t == "string")
        return PresetParam::Type::String;
    if (t == "internal")
        return PresetParam::Type::Internal;
    if (t == "color")
        return PresetParam::Type::Color;
    if (t == "bytearray")
        return PresetParam::Type::ByteArray;
    return PresetParam::Type::Unknown;
}

// ------------------------------------------------------------------------------------------------
// <Brush> helpers

void readMaskGenerator(pugi::xml_node el, AutoTipXml& out) {
    cb::MaskGeneratorParams& g = out.generator;

    // Legacy `radius=` is a synonym for `diameter=` -- the SAME value ("mistakenly named radius
    // for 2.2"), and it wins when both are present. Absent both: 1.0.
    if (const pugi::xml_attribute radius = el.attribute("radius"))
        g.diameter = doubleAttribute(radius, 1.0);
    else
        g.diameter = doubleAttribute(el.attribute("diameter"), 1.0);
    g.ratio = doubleAttribute(el.attribute("ratio"), 1.0);
    g.hFade = doubleAttribute(el.attribute("hfade"), 0.0);
    g.vFade = doubleAttribute(el.attribute("vfade"), 0.0);
    g.spikes = intAttribute(el.attribute("spikes"), 2, 2, 360);
    g.antialiasEdges = boolAttribute(el.attribute("antialiasEdges"), false);

    // `"circle"`-or-else-rect: there is no unknown shape, by the producer's own rule.
    g.shape = std::string_view(el.attribute("type").as_string("circle")) == "circle"
                  ? cb::MaskShape::Circle
                  : cb::MaskShape::Rect;

    // default / soft / else-GAUSS. An id from a newer or foreign producer loads as gauss --
    // that is the reference behaviour, not a guess -- but it is flagged for provenance.
    const std::string_view id = el.attribute("id").as_string("default");
    if (id == "default") {
        g.falloff = cb::MaskFalloff::Default;
    } else if (id == "soft") {
        g.falloff = cb::MaskFalloff::Soft;
    } else {
        g.falloff = cb::MaskFalloff::Gauss;
        out.unknownFalloffId = (id != "gauss");
    }

    // Only when PRESENT: an absent attribute keeps MaskGeneratorParams' descending default. The
    // identity curve would build an inside-out tip (§3.4), so it must not be the missing-value
    // fallback -- but a present value is taken verbatim, weird or not.
    if (const pugi::xml_attribute curve = el.attribute("softness_curve"))
        g.softnessCurve = cb::Curve::fromString(curve.value());
}

void readPredefined(pugi::xml_node el, PredefinedTipXml& out, std::string_view subtype) {
    out.filename = el.attribute("filename").as_string();
    out.md5sum = el.attribute("md5sum").as_string();

    out.scale = doubleAttribute(el.attribute("scale"), 1.0);
    // `BrushVersion` defaults to "1", and version 1 doubles -- so a tip element that omits the
    // attribute entirely has its scale doubled. Every reference in the default set writes "2"
    // (§3.5), which is exactly why getting this backwards stays invisible until a third-party
    // preset arrives.
    if (std::string_view(el.attribute("BrushVersion").as_string("1")) == "1")
        out.scale *= 2.0;

    // gbr (which includes the .gih hose) and png are the colour-capable loaders; svg and abr are
    // not, and for them the producer neither runs the legacy content test (always AlphaMask) nor
    // reads the three adjustments at all.
    out.colorfulCapable = (subtype == "gbr_brush" || subtype == "png_brush");

    if (out.colorfulCapable) {
        cb::TipAdjustments& adj = out.adjustments;
        adj.midPoint = intAttribute(el.attribute("AdjustmentMidPoint"), 127, 0, 255);
        adj.brightness = doubleAttribute(el.attribute("BrightnessAdjustment"), 0.0);
        adj.contrast = doubleAttribute(el.attribute("ContrastAdjustment"), 0.0);
        adj.autoMidPoint = boolAttribute(el.attribute("AutoAdjustMidPoint"), false);

        // The adjustment migration (§3.5): a version-1 element with no AutoAdjustMidPoint (i.e.
        // written before Krita 5) had its curve applied twice by the old renderer, and the blunt
        // undo is to double everything -- the midpoint about 127 (clamped), brightness and
        // contrast outright (deliberately NOT clamped, as the producer does not), and a negative
        // contrast remapped through the changed formula. Neutral values stay neutral, so the
        // whole default set is untouched.
        const int adjustmentVersion = intAttribute(el.attribute("AdjustmentVersion"), 1, 0, 1000);
        if (adjustmentVersion < 2 && !el.attribute("AutoAdjustMidPoint")) {
            adj.midPoint = std::clamp(127.0 + (adj.midPoint - 127.0) * 2.0, 0.0, 255.0);
            adj.brightness *= 2.0;
            adj.contrast *= 2.0;
            if (adj.contrast < 0.0)
                adj.contrast = 1.0 / (1.0 - adj.contrast) - 1.0;
        }
    }

    // The application rule, in the producer's own branch order (§3.5). preserveLightness first,
    // then an explicit brushApplication, then the two legacy shapes.
    if (const pugi::xml_attribute pl = el.attribute("preserveLightness")) {
        if (boolAttribute(pl, false)) {
            out.applicationRule = TipApplicationRule::ForceLightness;
        } else {
            out.applicationRule = TipApplicationRule::LegacyContentTest;
            out.colorAsMask = boolAttribute(el.attribute("ColorAsMask"), true);
        }
    } else if (const pugi::xml_attribute app = el.attribute("brushApplication")) {
        out.applicationRule = TipApplicationRule::Explicit;
        // ALPHAMASK=0, IMAGESTAMP=1, LIGHTNESSMAP=2, GRADIENTMAP=3 -- the serialized values,
        // double-checked in the source because two readings of the enum disagreed (§3.5). An
        // out-of-range int falls back to AlphaMask -- clamping would turn a foreign value into
        // GradientMap, the least likely intent of all four.
        const int v = intAttribute(app, 0, std::numeric_limits<int>::min(),
                                   std::numeric_limits<int>::max());
        out.application = (v >= 0 && v <= 3) ? static_cast<cb::TipApplication>(v)
                                             : cb::TipApplication::AlphaMask;
    } else if (const pugi::xml_attribute mask = el.attribute("ColorAsMask")) {
        out.applicationRule = TipApplicationRule::LegacyContentTest;
        out.colorAsMask = boolAttribute(mask, true);
    } else {
        // No attribute at all: the pre-4.4 automatic heuristic, colour deciding by itself.
        out.applicationRule = TipApplicationRule::LegacyContentTest;
        out.colorAsMask = false;
    }
}

} // namespace

std::optional<std::string> PresetXml::property(std::string_view key) const {
    const auto it = params.find(key);
    if (it == params.end())
        return std::nullopt;
    return it->second.value;
}

std::optional<PresetXml> parsePresetXml(std::string_view xml, std::string* error) {
    pugi::xml_document doc;
    if (!loadDocument(doc, xml, error))
        return std::nullopt;

    const pugi::xml_node root = doc.child("Preset");
    if (!root) {
        if (error != nullptr)
            *error = "no <Preset> element";
        return std::nullopt;
    }

    PresetXml out;
    out.name = root.attribute("name").as_string();
    out.paintopId = root.attribute("paintopid").as_string();

    std::size_t totalBytes = 0;
    for (pugi::xml_node param = root.child("param"); param;
         param = param.next_sibling("param")) {
        const std::string_view name = param.attribute("name").as_string();
        // child_value() folds CDATA and plain text alike; the shipped files use CDATA for
        // string-typed params and plain text elsewhere, and both must read the same.
        std::string value = param.child_value();
        if (name.empty() || value.size() > kMaxPresetValueBytes ||
            value.size() > kMaxPresetTotalBytes - totalBytes ||
            static_cast<int>(out.params.size()) >= kMaxPresetParams) {
            ++out.skippedParams;
            continue;
        }

        PresetParam entry;
        entry.type = paramType(param.attribute("type").as_string());
        if (entry.type == PresetParam::Type::Unknown)
            ++out.unknownParamTypes;
        entry.value = std::move(value);

        totalBytes += entry.value.size();
        const auto [it, inserted] = out.params.insert_or_assign(std::string(name),
                                                                std::move(entry));
        if (!inserted)
            ++out.duplicateParams; // the producer's property map overwrites too: last wins
    }

    for (pugi::xml_node res = root.child("resources").child("resource"); res;
         res = res.next_sibling("resource")) {
        std::string payload{cb::detail::trim(res.child_value())};
        if (payload.size() > kMaxPresetValueBytes ||
            payload.size() > kMaxPresetTotalBytes - totalBytes ||
            static_cast<int>(out.resources.size()) >= kMaxEmbeddedResources) {
            ++out.skippedResources;
            continue;
        }
        totalBytes += payload.size();
        EmbeddedResource r;
        r.type = res.attribute("type").as_string();
        r.md5sum = res.attribute("md5sum").as_string();
        r.name = res.attribute("name").as_string();
        r.filename = res.attribute("filename").as_string();
        r.base64 = std::move(payload);
        out.resources.push_back(std::move(r));
    }

    return out;
}

std::optional<TipXml> parseTipXml(std::string_view xml, std::string* error) {
    pugi::xml_document doc;
    if (!loadDocument(doc, xml, error))
        return std::nullopt;

    // The producer looks the element up by name rather than assuming it is the root, so a wrapped
    // <Brush> loads there; match that.
    const pugi::xml_node el = doc.child("Brush");
    if (!el) {
        if (error != nullptr)
            *error = "no <Brush> element";
        return std::nullopt;
    }

    TipXml out;
    out.type = el.attribute("type").as_string();
    if (out.type == "auto_brush")
        out.kind = TipXml::Kind::Auto;
    else if (out.type == "gbr_brush" || out.type == "abr_brush" || out.type == "png_brush" ||
             out.type == "svg_brush")
        out.kind = TipXml::Kind::Predefined;
    else
        out.kind = TipXml::Kind::Unknown;

    out.angle = doubleAttribute(el.attribute("angle"), 0.0); // radians (§3.5)
    // ⚠ The spacing default is the one place the two factories disagree: 1.0 on an auto element,
    // 0.25 on a predefined one. §3.5's "absent means 0.25" is the predefined half of the truth.
    out.spacing = doubleAttribute(el.attribute("spacing"),
                                  out.kind == TipXml::Kind::Auto ? 1.0 : 0.25);
    out.useAutoSpacing = boolAttribute(el.attribute("useAutoSpacing"), false);
    out.autoSpacingCoeff = doubleAttribute(el.attribute("autoSpacingCoeff"), 1.0);

    if (out.kind == TipXml::Kind::Auto) {
        out.autoTip.randomness = doubleAttribute(el.attribute("randomness"), 0.0);
        out.autoTip.density = doubleAttribute(el.attribute("density"), 1.0);
        readMaskGenerator(el.child("MaskGenerator"), out.autoTip);
    } else if (out.kind == TipXml::Kind::Predefined) {
        readPredefined(el, out.predefined, out.type);
    }

    return out;
}

} // namespace mosaic::io::brush
