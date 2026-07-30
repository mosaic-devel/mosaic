#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/text/text_model.hpp"  // FontRef

// The font-resolution seam (docs/type-tool.md §4.2). The core text stack shapes and rasterizes,
// but it must NOT depend on the platform layer (core is built before platform), so font lookup is
// an abstract interface here and the concrete OS backend -- fontconfig (Linux), CoreText (macOS),
// DirectWrite (Windows) -- injects through it (platform::FontDB). Tests supply a fake. By design
// nothing here hardcodes a family name: the default family and the fallback chain are whatever the
// host machine actually has (so the same TextBlock resolves correctly on any of the three OSes).
namespace mosaic::core::text {

// A resolved font face -- a file on disk plus the face index within it (TrueType Collections and
// variable fonts pack several). This is exactly what FreeType's FT_New_Face wants, so the shaper
// hands it straight through; the provider's whole job is FontRef (or a missing codepoint) -> this.
struct FontFace {
    std::string path;                         // absolute path to the font file
    int index = 0;                            // face index within the file (0 = a plain single face)
    std::map<std::string, float> variations;  // OpenType variable-axis settings to apply (tag->value)

    bool operator==(const FontFace&) const = default;
};

class FontProvider {
public:
    virtual ~FontProvider() = default;

    // Resolve a style request to a concrete face, honouring family / weight / italic / width.
    // Returns nullopt only when the machine has no usable font at all; otherwise the OS matcher
    // always yields its closest face (that is the intended graceful fallback to the default).
    [[nodiscard]] virtual std::optional<FontFace> resolve(const FontRef& ref) const = 0;

    // A face that COVERS `codepoint`, via the OS fallback cascade, biased toward `base`'s style --
    // for glyphs the chosen family lacks (CJK, emoji, symbols). nullopt if nothing installed covers it.
    [[nodiscard]] virtual std::optional<FontFace> fallbackFor(char32_t codepoint,
                                                              const FontRef& base) const = 0;

    // The OS default UI/sans family name (whatever `sans-serif` maps to on this machine). Empty
    // string only if the matcher fails entirely.
    [[nodiscard]] virtual std::string defaultFamily() const = 0;

    // Every installed family name, sorted and de-duplicated (for a font picker).
    [[nodiscard]] virtual std::vector<std::string> families() const = 0;

    // Families carrying colour glyphs -- COLR/CPAL or embedded bitmaps -- for the Settings emoji-
    // font picker (§4.2). May be empty on a machine with no colour fonts.
    [[nodiscard]] virtual std::vector<std::string> emojiFamilies() const = 0;

    // A short, representative sample string to PREVIEW `family` in the font picker -- rendered in the
    // family itself (so the user sees how it draws). Coverage-aware: a Latin (or Latin+X) face gets
    // "Abg"; a dedicated non-Latin face gets a sample in its own script (Cyrillic, CJK, Arabic…); a
    // colour/emoji face gets emoji. The base returns the Latin default; the OS backend overrides it
    // using its coverage data (fontconfig charset/colour). Never empty.
    [[nodiscard]] virtual std::string sampleTextFor(const std::string& /*family*/) const {
        return "Abg";
    }
};

}  // namespace mosaic::core::text
