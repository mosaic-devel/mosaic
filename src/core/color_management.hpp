#pragma once

#include "common/image.hpp" // Color8
#include "core/document.hpp" // ColorSpace

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// lcms2-backed colour management for one working colour space (PLAN S12-b). The engine converts
// between the working space's gamma-encoded RGB and CIELAB (D50, the ICC profile connection
// space), which is what the picker's Lab model and the out-of-gamut warning are built on: a Lab
// value transformed to *unclamped* float RGB is out of gamut exactly when a channel leaves [0, 1].
//
// lcms2 is a PRIVATE implementation detail (the nlohmann/json rule): this header is lcms2-free,
// the handles live behind the pimpl. Profiles for every core::ColorSpace are built in-memory from
// primaries + transfer curves -- no ICC files are shipped or loaded. (That is also why CMYK is NOT
// here yet: lcms2 has no built-in CMYK profile; an honest managed CMYK needs a real press profile,
// e.g. FOGRA39 -- see PLAN §2.)
namespace mosaic::core {

// CIELAB (D50). L in [0, 100]; a/b nominally within about [-128, 127].
struct Lab {
    float l = 0.0F;
    float a = 0.0F;
    float b = 0.0F;
};

// Unclamped working-space RGB as nominal-[0, 1] floats; values outside that range encode
// out-of-gamut colours (unbounded lcms2 float transform).
struct RgbF {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
};

// Device CMYK in ink percentages [0, 100], meaningful only against a concrete press profile.
struct Cmyk {
    float c = 0.0F;
    float m = 0.0F;
    float y = 0.0F;
    float k = 0.0F;
};

class ColorEngine {
public:
    explicit ColorEngine(ColorSpace working);
    ~ColorEngine();
    ColorEngine(const ColorEngine&) = delete; // owns lcms2 handles
    ColorEngine& operator=(const ColorEngine&) = delete;

    [[nodiscard]] ColorSpace workingSpace() const noexcept { return m_working; }

    [[nodiscard]] Lab toLab(common::Color8 rgb) const;
    [[nodiscard]] RgbF toRgbUnclamped(Lab lab) const; // gamut test: any channel outside [0, 1]
    [[nodiscard]] common::Color8 toRgbClamped(Lab lab) const; // the "snap to nearest" colour

    // Inside [0, 1] with a small epsilon for float/8-bit rounding slack.
    [[nodiscard]] static bool inGamut(const RgbF& c) noexcept;

    // CMYK (S12-b): managed against a real press profile. The vendored FOGRA39-based
    // ISOcoated_v2_300_eci.icc (third_party/icc-profiles/, HEIDELBERG licence) is loaded as the
    // default at construction; S12-c lets the user substitute their own. hasCmyk() is false only
    // if loading failed (e.g. a build with the profile stripped) -- callers hide CMYK UI then.
    [[nodiscard]] bool hasCmyk() const noexcept;
    bool loadCmykProfile(const void* data, std::size_t size); // false + unchanged on bad data
    bool loadCmykProfileFile(const char* path);               // ditto (S12-c user profiles)
    // Reload the vendored built-in default press profile, reverting a user override at runtime
    // (the "Use default" path in Settings). false if no default is compiled in or it failed.
    bool loadDefaultCmykProfile();

    // Replace the working space with a user-supplied RGB .icc (S12-c). Rebuilds the Lab *and*
    // CMYK transforms (both are anchored to the working profile). false + unchanged on bad data
    // (missing file, or a profile whose colour space is not RGB).
    bool loadWorkingProfileFile(const char* path);

    // Display name for the working space: the built-in enum's name, or — after
    // loadWorkingProfileFile — the profile's embedded description tag.
    [[nodiscard]] std::string workingName() const;
    [[nodiscard]] Cmyk toCmyk(common::Color8 rgb) const;      // requires hasCmyk()
    [[nodiscard]] common::Color8 cmykToRgb(Cmyk c) const;     // requires hasCmyk()

private:
    struct Impl;
    ColorSpace m_working;
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------------------------
// Profiles as bytes, for embedding in an exported file (Export & I/O plan §6's Colour row)
// ---------------------------------------------------------------------------------------------

// The built-in profile for `cs`, serialised as a complete in-memory .icc.
//
// The same primaries + transfer curve the engine converts through, written out so a PNG/JPEG/WebP
// can carry them: a document whose working space is Display P3 must not export as an untagged file
// that every viewer then reads as sRGB. No profile files are shipped for this -- the profile is
// BUILT, exactly as the engine's own is, so the file a document embeds and the numbers the picker
// computed can never disagree. The description tag is overwritten with the space's own name, so
// another application shows "Display P3" rather than lcms2's generic "RGB built-in".
//
// Empty on any failure (which in practice means lcms2 could not allocate).
[[nodiscard]] std::vector<std::uint8_t> workingSpaceIccProfile(ColorSpace cs);

// The profile an export of `doc` should embed, or empty for none.
//
//   * a custom working profile (File ▸ New's "Custom…" ICC entry, Document::iccProfilePath) is
//     read, VERIFIED TO BE AN RGB PROFILE, and re-serialised through lcms2 -- which is a real
//     parse, not the two structural checks io::readIccProfile can afford, and it also normalises
//     whatever the file happened to contain into something a codec will accept;
//   * otherwise the built-in profile for colorSpace() (above);
//   * except for plain sRGB, which returns EMPTY on purpose: sRGB is what an untagged file is read
//     as everywhere, so embedding three kilobytes to say so in every export would be cost with no
//     benefit. A user who genuinely wants the tag can pick the profile explicitly in the modal.
//
// A custom profile that will not open, or that is not RGB, falls back to the working space's
// built-in rather than failing -- an unusable profile costs the tag, never the export.
[[nodiscard]] std::vector<std::uint8_t> documentIccProfile(const Document& doc);

// The embedded description tag of the .icc at `path`, e.g. "ISO Coated v2 300% (ECI)" -- for
// showing a user-chosen profile's real name in the UI. Reads the file directly, independent of
// any ColorEngine state; returns "" if it cannot be opened. Works for any profile class.
[[nodiscard]] std::string iccProfileName(const std::string& path);

// Historical alias (S12-c call sites); same reader.
[[nodiscard]] std::string cmykProfileName(const std::string& path);

// The built-in (vendored) default CMYK press profile's embedded description tag, e.g.
// "ISO Coated v2 300% (ECI)" -- for showing the user *which* profile "Use default" means. Reads the
// compiled-in profile independently of any ColorEngine/override state; returns "" if no default is
// compiled in or it could not be read (a build with the profile stripped).
[[nodiscard]] std::string defaultCmykProfileName();

} // namespace mosaic::core
