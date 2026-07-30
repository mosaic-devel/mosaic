#pragma once

#include "common/exif.hpp"
#include "common/image.hpp"
#include "io/document_profile.hpp"
#include "io/export_path.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class Fl_Window; // forward-declared: this header stays free of FLTK includes.

// The Export As… modal (Export & I/O plan §6): a two-pane dialog over the already-flattened
// document -- a big INTERACTIVE preview on the left (wheel zoom about the cursor, drag to pan,
// 1:1 / fit in its lower-right corner) and, on the right, a scrolling settings column: presets +
// format + the format's own options + output size + colour + the live loss banner, with the
// output path and the action buttons on the bar beneath both.
//
// M3 re-plumbed it onto the M2 framework, which is what makes it scale past three formats: the
// format list is FormatRegistry::exportOrder(), the options panel is RENDERED FROM the backend's
// OptionsSchema (no per-format panel code), the encode goes through FormatBackend::encode(), the
// banner is io::diff() translated by LossCode, and the preview + the exact file size both come
// from one real trial encode run off the UI thread (the §5 three-stage cache).
//
// An Export is NOT a Save: the caller never clears the command-stack saved marker.
namespace mosaic::ui {

// Everything the modal needs to know about the document. Passed as one struct because M3 added
// three fields at once and every one of them is load-bearing for a rule in §6.
struct ExportRequest {
    // The whole-document flatten (8-bit straight-alpha RGBA, checkerboard off) the app already
    // renders for Quick Export. Required; an empty image opens nothing.
    const common::Image* composited = nullptr;
    // What the document actually uses -- io::profileDocument(doc, *composited). Drives the live
    // loss banner. Profiling with the flatten in hand is what makes hasAlpha / distinctColors
    // exact rather than conservative (io/document_profile.hpp).
    io::DocumentProfile profile;
    // This document's sticky export memory (§6 "Path behavior"): empty on the first export, then
    // the last path + format so re-export is one click. It is per-document APP STATE, never a
    // sidecar beside the user's file, and it must never leak to the next document.
    io::ExportTarget target;
    std::string suggestedStem;  // file stem to offer when there is no remembered target
    std::string documentPath;   // core::Document::filePath(); seeds the picker's start folder
    double dpi = 72.0;

    // ---- what the Colour & metadata section has to offer (M5) ----
    //
    // The two payloads §6 promised and M4 left unwired. Both are RESOLVED BY THE CALLER, once:
    // the provenance rule that decides which layer's EXIF an export writes lives in
    // io::documentExif, and turning a working space or a custom .icc into embeddable bytes lives
    // in core::documentIccProfile -- neither belongs in a dialog, and re-deriving them per trial
    // encode would put file I/O on the modal's worker thread.

    // The camera metadata an export of this document may write back, already normalised (its
    // orientation reads 1). nullopt = the document carries none, and the Metadata row says so
    // instead of offering a switch with nothing behind it.
    std::optional<common::ExifData> exif;
    // The document's own colour profile as complete .icc bytes, and the name to show for it.
    // Empty = the document is plain sRGB, which needs no tag (core::documentIccProfile).
    std::vector<std::uint8_t> documentIcc;
    std::string documentIccName;

    // Settings -> General -> "Show all export formats" (§0/§3). False -- the default -- offers the
    // Common and curated-pro tiers; true also offers the exotic tier, which is empty until M7, so
    // today this changes nothing visible and is wired so that M7 changes no UI code.
    bool showAllFormats = false;
};

// The outcome of a completed export. Absent when the user cancelled. The caller reports `path`
// and stores both fields back into the document's io::ExportTarget.
struct ExportResult {
    std::string path;
    std::string formatId;    // io::formatIdName() of the backend that wrote it
    io::OptionValues values; // the options it was written with, so re-export repeats them exactly
};

// Show the modal Export dialog. Centred over `host`. Returns what was written, or nullopt if the
// user cancelled or nothing was written. Requires a display.
[[nodiscard]] std::optional<ExportResult> showExportDialog(const ExportRequest& request,
                                                           Fl_Window* host = nullptr);

// ---- the modal's pure arithmetic (unit-tested in tests/test_export_dialog.cpp) ----------------
//
// Small, but they are what the info line and the three linked size fields actually SAY, and a
// dialog is the one place a rounding slip is invisible until a user reports a 1919-pixel export.

// The info line's byte count ("864 B", "12.4 KB", "2.31 MB"). Binary units, matching every
// other size readout in the app.
[[nodiscard]] std::string humanFileSize(std::size_t bytes);

// The output dimension cap, matching io::detail::kMaxDim (the decoder's own sanity limit).
inline constexpr std::uint32_t kMaxExportDim = 30000;

struct ExportPixelSize {
    std::uint32_t w = 1;
    std::uint32_t h = 1;
    bool operator==(const ExportPixelSize&) const = default;
};

// The three ways the size section can be driven. Every one of them clamps into
// [1, kMaxExportDim] on BOTH axes, so no entry -- a pasted 1e9, a 0, a negative -- can produce a
// dimension the resize or the encoder would choke on.
[[nodiscard]] ExportPixelSize exportSizeFromScale(std::uint32_t baseW, std::uint32_t baseH,
                                                  double percent);
[[nodiscard]] ExportPixelSize exportSizeFromWidth(std::uint32_t baseW, std::uint32_t baseH,
                                                  double width, std::uint32_t currentH,
                                                  bool lockAspect);
[[nodiscard]] ExportPixelSize exportSizeFromHeight(std::uint32_t baseW, std::uint32_t baseH,
                                                   double height, std::uint32_t currentW,
                                                   bool lockAspect);

// The percentage the scale field/slider shows for an output width (100 when the source is empty).
[[nodiscard]] double exportScalePercent(std::uint32_t baseW, std::uint32_t outW);

} // namespace mosaic::ui
