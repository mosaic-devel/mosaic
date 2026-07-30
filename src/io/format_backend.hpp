#pragma once

#include "common/exif.hpp"
#include "common/image.hpp"
#include "io/caps.hpp"
#include "io/options_schema.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// io/format_backend -- the interface every export format implements (Export & I/O plan §2.1).
//
// This is the abstraction that makes "as advanced as they get" scale to ~40 formats without 40
// bespoke UI panels: a backend is three pieces of PURE DATA (caps, options schema, identity)
// plus one encode entry point. The modal reads the data and renders itself; nothing in ui/ ever
// learns a format's name. Adding a format is one file here plus one line in the registry.
namespace mosaic::io {

// The formats the registry can name. An id is just a name -- declaring one does NOT imply a
// backend exists (FormatRegistry only ever holds backends that were actually built). The set
// covers §3's Tier 1 (Common) and Tier 2 (Curated pro); Tier 3 (exotic) joins at M7.
//
// HEIC/HEVC is deliberately absent: Mosaic ships no HEVC code, so there is nothing for an id to
// refer to. That is a standing decision, not a gap waiting to be filled.
enum class FormatId : std::uint16_t {
    // Tier 1 -- Common, in the combobox's popularity order
    Png,
    Jpeg,
    Jxl,
    WebP,
    Gif,
    Tiff,
    Avif,
    Pdf,
    Svg,
    // Tier 2 -- Curated pro, always on, alphabetical below the divider
    Bmp,
    Eps,
    Ico,
    Jpeg2000,
    OpenExr,
    Pnm,
    Qoi,
    RadianceHdr,
    Tga,
};

// Stable, untranslated identity for an id -- the preset key and the test oracle.
[[nodiscard]] std::string_view formatIdName(FormatId id) noexcept;

// §0/§3: the curated pro set is always on; the exotic tier hides behind
// Settings -> General -> "Show all export formats".
enum class FormatTier : std::uint8_t { Common, CuratedPro, Exotic };

// What encode() is handed: the FINISHED, ALREADY-RESIZED flatten plus the side-car information
// a format may be able to carry. The resize happens before this point on purpose -- see the
// three-stage cache in §5 (composite -> resize -> encode); io never resizes and never composites.
struct RenderInput {
    const common::Image* pixels = nullptr;  // 8-bit straight-alpha RGBA; required

    double dpi = 72.0;
    // Fills transparency for a format with AlphaKind::None (the modal's "Matte" row, §6.6).
    common::Color8 matte{255, 255, 255, 255};
    // Metadata to write back, when the backend and the user's Metadata toggle both allow it.
    // The record an EXPORT may write, not the one a LOAD produced: orientation has to read 1
    // here (io::exifForExport), because the load already baked the rotation into the pixels.
    const common::ExifData* exif = nullptr;
    // The colour profile to embed, as the complete bytes of a .icc file. Empty = embed none, and
    // the file is then read as plain sRGB everywhere.
    //
    // BYTES rather than a path, which is what this field used to be, for two reasons. The profile
    // a document exports is not always a file: a working space with no custom .icc is serialised
    // out of lcms2 in memory (core::documentIccProfile). And the trial encode behind the modal's
    // size readout runs on a WORKER THREAD, off a debounced keystroke -- the wrong place to be
    // re-reading a two-megabyte press profile, and a worse place to be discovering that its path
    // stopped resolving. Resolve and validate once, at the UI, then hand the bytes down.
    std::vector<std::uint8_t> iccProfile;
    bool stripMetadata = false;  // the privacy toggle: write no EXIF/XMP/ICC at all
};

struct EncodeResult {
    bool ok = false;
    std::string error;                 // human-readable, only meaningful when !ok
    std::vector<std::uint8_t> bytes;   // the encoded file

    [[nodiscard]] static EncodeResult failure(std::string why) {
        EncodeResult r;
        r.error = std::move(why);
        return r;
    }
};

// Progress in [0,1]. Return false to CANCEL the encode (§5: a 100 MP JXL at effort 9 must not
// block a slider). An empty function means "no reporting, never cancelled".
using ProgressFn = std::function<bool(float)>;

class FormatBackend {
public:
    virtual ~FormatBackend() = default;

    FormatBackend(const FormatBackend&) = delete;
    FormatBackend& operator=(const FormatBackend&) = delete;

    [[nodiscard]] virtual FormatId id() const noexcept = 0;
    [[nodiscard]] virtual FormatTier tier() const noexcept = 0;
    // English source string ("PNG image"); the combobox translates it.
    [[nodiscard]] virtual std::string_view displayName() const noexcept = 0;
    // Lower-case, without the dot; [0] is the primary one the Save dialog appends.
    [[nodiscard]] virtual std::vector<std::string> extensions() const = 0;
    [[nodiscard]] virtual std::string_view mimeType() const noexcept = 0;

    [[nodiscard]] virtual FormatCaps caps() const = 0;
    [[nodiscard]] virtual OptionsSchema optionsSchema() const = 0;

    // Runtime probe: codec availability is build- AND runtime-dependent (libjxl may be absent,
    // TIFFIsCODECConfigured() answers per codec, libavif's codec list varies). The combobox is
    // populated from this, so an unavailable backend is simply not offered.
    [[nodiscard]] virtual bool available() const noexcept { return true; }

    // Encode `input` with `values` (which SHOULD have been through
    // optionsSchema().coerce(); a backend must still not crash on anything else).
    [[nodiscard]] virtual EncodeResult encode(const RenderInput& input, const OptionValues& values,
                                              const ProgressFn& progress = {}) const = 0;

protected:
    FormatBackend() = default;
};

// Encode and write to `path`. Kept out of the interface so a backend implements ONE thing: the
// bytes. (§5 also wants those bytes for the preview and the exact-size readout, so producing
// them is never wasted work.)
[[nodiscard]] bool encodeToFile(const FormatBackend& backend, const RenderInput& input,
                                const OptionValues& values, const std::string& path,
                                std::string* error = nullptr);

} // namespace mosaic::io
