#pragma once

#include <cstdint>
#include <memory>

#include "core/text/font_provider.hpp"

// The Linux (fontconfig) implementation of the cross-platform font seam (docs/type-tool.md §4.2).
// macOS (CoreText) and Windows (DirectWrite) backends are separate files added with those ports;
// the app constructs whichever one matches the host and hands it to the text stack as a
// core::text::FontProvider. Nothing platform-specific leaks past this header.
namespace mosaic::platform {

class FontDB : public core::text::FontProvider {
public:
    FontDB();
    ~FontDB() override;
    FontDB(const FontDB&) = delete;
    FontDB& operator=(const FontDB&) = delete;

    [[nodiscard]] std::optional<core::text::FontFace> resolve(
        const core::text::FontRef& ref) const override;
    [[nodiscard]] std::optional<core::text::FontFace> fallbackFor(
        char32_t codepoint, const core::text::FontRef& base) const override;
    [[nodiscard]] std::string defaultFamily() const override;
    [[nodiscard]] std::vector<std::string> families() const override;
    [[nodiscard]] std::vector<std::string> emojiFamilies() const override;
    [[nodiscard]] std::string sampleTextFor(const std::string& family) const override;

    // Prefer `family` when a fallback face is needed for an EMOJI codepoint (Settings → Text →
    // Emoji font, R5 docs/type-tool.md §4.2). Empty = automatic (the plain fontconfig cascade).
    // Only a preference: a codepoint the family doesn't actually cover falls through to the
    // normal cascade, so a bad pick never produces tofu.
    void setPreferredEmojiFamily(std::string family);

    // ⚠ How many fontconfig MATCHES have actually run, ever (resolve + fallback cache misses).
    // An EVENT count, monotonic -- the only honest way to assert the memoization holds: a cache
    // SIZE refills after a wrongly-dropped cache and a mutant sails through it (the icon-cache
    // lesson, brush dock). resolve()/fallbackFor() are memoized because shaping consults them PER
    // RUN PER LAYOUT: an uncached FcFontMatch is milliseconds, and it made every keystroke, bend
    // tick and font-hover pay fontconfig again ("bending text is extremely laggy", user
    // 2026-07-14). setPreferredEmojiFamily invalidates the fallback cache (it changes answers);
    // nothing else does -- a font installed mid-session is picked up on restart, as before.
    [[nodiscard]] std::uint64_t fcMatches() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::platform
