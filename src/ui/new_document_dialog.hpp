#pragma once

#include "common/image.hpp"  // common::Image (recent/template card thumbnails)
#include "core/document.hpp" // core::ColorSpace, core::Precision, core::Document

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Fl_Window; // forward-declared: the headless-tested parts of this header stay FLTK-free

// The File -> New dialog (PLAN S9, redesigned when S55 landed here): a category rail (Recent /
// Print / Screen / Templates) over a card gallery, a settings form with a live document summary
// (pixel size + per-layer memory), and a New-from-Clipboard action. It returns a CHOICE -- a
// blank document spec, a template to instantiate, a recent file to open, or "from clipboard" --
// and the app dispatches each. The size math, document construction, preset/template models, and
// the memory estimate are deliberately split out of the FLTK dialog so they are unit-tested
// headlessly; only showNewDocumentDialog() needs a display.
namespace mosaic::ui {

// Units offered for the custom width/height. Pixels are resolution-independent; the physical
// units convert to pixels through the document resolution (DPI).
enum class SizeUnit { Pixels, Millimeters, Centimeters, Inches, Points };
[[nodiscard]] std::string_view sizeUnitName(SizeUnit u);   // "Pixels", "Millimeters", ...
[[nodiscard]] std::string_view sizeUnitAbbrev(SizeUnit u); // "px", "mm", ... (in-field unit tags)

// How the new document's single background layer is filled.
enum class NewDocBackground { White, Black, Transparent };
[[nodiscard]] std::string_view newDocBackgroundName(NewDocBackground b);

// Largest canvas side we allow. Matches a typical GPU max 2D image dimension and keeps the
// one-shot CPU composite (no tiling until S60) bounded; pixel dimensions clamp to [1, this].
inline constexpr std::uint32_t kMaxCanvasDimension = 16384;

// Convert a length in `unit` to pixels at `dpi` (pixels pass through). Pure; exposed for tests.
[[nodiscard]] double unitToPixels(double value, SizeUnit unit, double dpi);
// Inverse: express a pixel length in `unit` at `dpi` (used when the unit dropdown changes).
[[nodiscard]] double pixelsToUnit(double pixels, SizeUnit unit, double dpi);

// A fully-resolved description of the document to create. The dialog produces one of these;
// buildDocument() turns it into a real core::Document.
struct NewDocumentSpec {
    std::string title = "Untitled";
    double width = 1920.0;  // in `unit`
    double height = 1080.0; // in `unit`
    SizeUnit unit = SizeUnit::Pixels;
    double dpi = 72.0;
    core::ColorSpace colorSpace = core::ColorSpace::SRGB;
    core::Precision precision = core::Precision::U8;
    NewDocBackground background = NewDocBackground::White;
    // Non-empty = the Color dropdown's "Custom..." pick: an RGB .icc governing the document's
    // working space (colorSpace stays the fallback where the file is unavailable). Round 5.
    std::string iccProfilePath;

    // width/height resolved to integer pixels at `dpi`, clamped to [1, kMaxCanvasDimension].
    [[nodiscard]] std::uint32_t pixelWidth() const;
    [[nodiscard]] std::uint32_t pixelHeight() const;
};

// The default seed for the dialog (Full HD, sRGB, 8-bit, white background).
[[nodiscard]] NewDocumentSpec defaultNewDocumentSpec();

// The gallery's preset split (the dialog's rail): paper sizes are Print, display sizes Screen,
// power-of-two squares Texture.
enum class PresetCategory { Print, Screen, Texture };

// A named entry in the preset list. `name` points at a static string (shown on the cards).
struct DocumentPreset {
    std::string_view name;
    double width;
    double height;
    SizeUnit unit;
    double dpi;
    PresetCategory category;
};

// The built-in presets: ISO 216 A0-A5, US Letter/Legal/Tabloid (Print), common display sizes
// (Screen), and 128..8192 power-of-two squares (Texture).
[[nodiscard]] const std::vector<DocumentPreset>& documentPresets();

[[nodiscard]] PresetCategory presetCategory(const DocumentPreset& p);

// Split a preset name for its gallery card: "A4  (210 × 297 mm)" -> short name "A4" (drawn on the
// paper face) and detail "210 × 297 mm" (the subtitle). A name with no "  (" detail is all short.
[[nodiscard]] std::string_view presetShortName(const DocumentPreset& p);
[[nodiscard]] std::string presetDetail(const DocumentPreset& p);

// The preset `spec` corresponds to (size + unit + dpi, orientation-blind), or -1 (custom).
// The app records a created Blank spec as a custom-size recent exactly when this says -1.
[[nodiscard]] int matchDocumentPreset(const NewDocumentSpec& spec);

// ---- custom-size recents (the Recent gallery's "Sizes" section, user round 5) ---------------

// A hand-entered size that matches no preset is remembered in Settings::recentSizes as a token
// "W;H;<unit abbrev>;DPI" ('.' decimals, locale-independent). parse returns nullopt for anything
// malformed, non-positive, or with an unknown unit -- a bad settings entry simply drops out.
[[nodiscard]] std::string customSizeToken(const NewDocumentSpec& spec);
[[nodiscard]] std::optional<NewDocumentSpec> parseCustomSizeToken(const std::string& token);

// Display strings for a custom-size card: title "1920 × 1080 px" (trimmed decimals), and the
// short numeric face "1920 × 1080" drawn on the sheet.
[[nodiscard]] std::string customSizeTitle(const NewDocumentSpec& spec);
[[nodiscard]] std::string customSizeFace(const NewDocumentSpec& spec);

// The Name the next File -> New dialog offers: "Untitled", or the first "Untitled N" (N >= 2)
// not already used by an open document -- so two fresh documents never share a title (round 5).
[[nodiscard]] std::string nextUntitledTitle(const std::vector<std::string>& openTitles);

// True exactly for the titles nextUntitledTitle generates ("Untitled", "Untitled 2", ...): the
// auto names a save may ADOPT the chosen file stem over (round 7) -- a deliberate title never
// qualifies, so it is never clobbered.
[[nodiscard]] bool isAutoUntitledTitle(std::string_view title);

// ---- document summary (the form's live readout) --------------------------------------------

// Bytes per RGBA pixel at a precision: U8 = 4, U16/F16 = 8, F32 = 16.
[[nodiscard]] std::size_t bytesPerPixel(core::Precision p);

// One layer's uncompressed pixel storage for the spec'd canvas (pixelWidth × pixelHeight ×
// bytesPerPixel) -- the "size × precision cost" surfaced before the document exists, scaling
// with every layer the user will add.
[[nodiscard]] std::uint64_t layerMemoryBytes(const NewDocumentSpec& spec);

// Human-readable decimal size: "612 B", "45.1 KB", "33.2 MB", "1.1 GB".
[[nodiscard]] std::string formatByteSize(std::uint64_t bytes);

// ---- document templates (data/presets, PLAN S55; the installed path arrives with S59) ------

// One template file: "2-Resume.mosaic" -> order 2, name "Resume" (underscores read as spaces).
// A file with no leading "<number>-" sorts after every numbered one, alphabetically.
struct TemplateFile {
    std::filesystem::path path;
    std::string name;
    int order = 0;
    bool operator==(const TemplateFile&) const = default;
};

// The *.mosaic files of `dir`, ordered by (order, case-insensitive name). A missing directory is
// simply zero templates. Callers scan installedDataDir()/"presets" (shipped) and
// dataDir()/"presets" (user), concatenating shipped-first -- the brush-preset precedent.
[[nodiscard]] std::vector<TemplateFile> scanDocumentTemplates(const std::filesystem::path& dir);

// A file's parent folder for display, with `homeDir` (when non-empty and a prefix) abbreviated
// to "~": "/home/u/Pictures/a.png" -> "~/Pictures". The recents cards' location line.
[[nodiscard]] std::string abbreviatedLocation(const std::string& absolutePath,
                                              const std::string& homeDir);

// Build a document from a spec: a canvas at spec.pixel{Width,Height}() with the chosen colour
// space / precision / DPI, plus one background raster layer -- filled white or black (and locked,
// the conventional opaque background), or an empty transparent layer so the checkerboard shows.
[[nodiscard]] std::unique_ptr<core::Document> buildDocument(const NewDocumentSpec& spec);

// ---- the dialog's inputs and result --------------------------------------------------------

// The size a recent/template card thumbnail is pre-fitted to by the CALLER (the card blits 1:1;
// see GalleryCard). The fit helpers below do the fitting (moved here when the S55 thumbnail
// cache died in favour of .mosaic PRVW chunks + the desktop's shared thumbnail cache).
inline constexpr int kNewDocCardThumbW = 122;
inline constexpr int kNewDocCardThumbH = 92;

// Fit (srcW, srcH) inside (maxW, maxH) preserving aspect, never upscaling; {0,0} for empty input.
struct FitSize {
    int width = 0;
    int height = 0;
};
[[nodiscard]] FitSize fitPreservingAspect(std::uint32_t srcW, std::uint32_t srcH, int maxW,
                                          int maxH);

// Area-averaged (box) downscale of an RGBA image to exactly (dstW, dstH). Straight per-channel
// means -- callers checkerCompose() first, so alpha is 255 and no premul care is needed.
// Upscaling is not supported: callers fit first.
[[nodiscard]] common::Image boxDownscale(const common::Image& src, int dstW, int dstH);

// Composite an RGBA image over the light/dark checker (the compositor's transparency language),
// yielding an opaque image the card blit needs no blending for.
[[nodiscard]] common::Image checkerCompose(const common::Image& src);

// One recent-file or template card, fully prepared by the caller (the dialog never touches the
// filesystem): a display title, a pre-formatted detail line, and an optional pre-fitted opaque
// thumbnail (empty = placeholder art).
struct NewDocumentCard {
    std::string path;     // absolute source path (returned in the choice)
    std::string title;    // card title -- template name / file name
    std::string subtitle; // the card's second line -- dims for templates, the LOCATION for
                          // recents (user 2026-07-22: the path must be visible)
    std::string detail;   // extra line for the summary panel on selection (dims; may be empty)
    common::Image thumb;  // pre-fitted to kNewDocCardThumb{W,H}; empty -> placeholder
    // Best-effort real values for the (disabled) form when this card is selected: the source
    // document's px size / ppi / colour space / bit depth where known, defaults elsewhere --
    // so the greyed form shows the document you would get, not leftover blank-form values.
    std::optional<NewDocumentSpec> values;
};

// Everything the dialog shows beyond the built-in presets.
struct NewDocumentContext {
    std::vector<NewDocumentCard> recents;   // newest first (Settings::recentFiles order)
    std::vector<NewDocumentCard> templates; // scanDocumentTemplates order
    // Hand-entered sizes remembered from past creates (Settings::recentSizes, newest first);
    // the Recent gallery's "Sizes" section. Only width/height/unit/dpi are meaningful.
    std::vector<NewDocumentSpec> customSizes;
    // The Clipboard card (leads the Recent gallery when present): the caller pre-fetched the OS
    // clipboard image, so the card shows a REAL preview and the image's exact pixel size.
    bool clipboardHasImage = false;
    common::Image clipboardThumb;  // pre-fitted like every card thumb (may be empty)
    std::string clipboardSubtitle; // "1234 × 567 px"
    std::optional<NewDocumentSpec> clipboardValues; // px size + best-effort defaults (72 ppi...)
    // Housekeeping (round 7; either may be null): fired when the user takes an entry off a
    // remembered list via a card's right-click menu, so the caller updates what it persists.
    // The dialog edits its own copy of the lists for the live gallery rebuild.
    std::function<void(const std::string& path)> onRemoveRecentFile;
    std::function<void(const NewDocumentSpec& size)> onForgetRecentSize;
};

// What the user chose. Blank carries the full spec; Template carries the file to instantiate
// plus spec.title (the Name field) for the new document; RecentFile carries the file to open;
// Clipboard means "create a document from the image the caller pre-fetched" (spec.title names it).
struct NewDocumentChoice {
    enum class Kind { Blank, Template, Clipboard, RecentFile };
    Kind kind = Kind::Blank;
    NewDocumentSpec spec;
    std::string path; // Template / RecentFile
};

// Show the modal dialog seeded from `initial` (defaults if null), centred over `host` (the
// pointer's screen when null). Returns the choice, or nullopt if the user cancelled.
// Requires a display; callers build/open + present the document. `ctx` moves in by value: the
// dialog edits its own copy live when a card's right-click menu removes an entry (round 7).
[[nodiscard]] std::optional<NewDocumentChoice> showNewDocumentDialog(
    NewDocumentContext ctx, const NewDocumentSpec* initial = nullptr, Fl_Window* host = nullptr);

} // namespace mosaic::ui
