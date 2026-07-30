#pragma once

#include "common/image.hpp"
#include "core/brush/bitmap_tip.hpp"
#include "core/brush/brush_engine.hpp"
#include "io/brush/bundle.hpp"
#include "io/brush/preset.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The preset library (docs/brushes.md §7): scan a .bundle, read every .kpp, resolve every
// predefined tip reference to its file, and finish the two decisions the mapper had to stage
// (preset.hpp): the §3.5 application rule for LegacyContentTest presets -- whose input is the
// tip IMAGE's content -- and the accumulator choice that follows from it. Masking resolves here
// too, because a predefined primary's absolute size (scale x max(w,h)) is only known once its
// file is decoded.
//
// Tip resolution reproduces the upstream bestMatch order (§3.5): the md5 INDEX first -- the
// digest is computed over each brushes/ file's bytes, never trusted from the manifest -- then
// the filename as the fallback, with the md5-miss recorded as a provenance note (upstream warns
// on exactly this). A reference resolving to nothing keeps the preset USABLE on the mapper's
// default round tip, badged, never dropped.
//
// Icons are deliberately lazy: a scan touches every file; a 200x200 RGBA thumbnail per preset
// is ~160 KB that only the presets a panel shows should cost.
//
// ⚠ But LAZY IS NOT THE SAME AS CHEAP, and it was not. loadIcon() re-read the whole source file,
// re-walked its central directory and re-parsed its manifest -- PER ICON. For Krita_4 that is a
// 17.5 MB read, a 285-entry directory walk and a 66 KB XML DOM to fetch one 200x200 PNG, and a
// dock-width drag asked for a dozen of them a frame. The archive is now opened ONCE per source, on
// the first icon fetched from it, and retained: an icon fetch is an inflate. The cost is the
// source's bytes held resident (ZipReader is a NON-OWNING view, zip.hpp, so they must be), which
// is the same 17.5 MB the old path read every single time.
namespace mosaic::io::brush {

enum class TipResolution : std::uint8_t {
    AutoTip,    // procedural -- nothing to resolve
    ByMd5,      // the md5 index hit (upstream's priority)
    ByFilename, // md5 absent or missed; the filename matched -- carries a provenance note
    Fallback,   // nothing matched or the file would not decode: the default round tip, badged
};

struct LibraryPreset {
    BrushPreset preset; // finalized: application + accumulator re-run against the loaded tip
    // The resolved bitmap tip; null when the tip is procedural (or fell back to one). Shared:
    // presets referencing the same file with the same application/adjustments share one build.
    std::shared_ptr<const core::brush::BitmapTip> tip;
    core::brush::MaskingParams masking; // resolved against the primary's absolute diameter
    // The TEXTURE option (§6.6h), resolved: the embedded pattern decoded and baked into the 8-bit
    // mask a dab reads. Disabled when the preset carries no texturing, or when its pattern would
    // not decode (badged). Patterns are SHARED across presets that agree on the payload and every
    // bake parameter -- five shipped presets embed the same dotted paper.
    core::brush::TextureParams texture;
    // The primary tip's ABSOLUTE size in document px -- the number a stroke is actually laid at, and
    // the one UseMasterSize multiplies. An auto tip carries it; a predefined one only yields it once
    // its file is decoded (`scale` x max(w, h) of the first frame, §3.5), which is why it is resolved
    // here and stored rather than re-derived by every consumer. `preset_brush.hpp` is the consumer
    // that would otherwise have had to reproduce this rule, and drift from it.
    double masterDiameter = 24.0;
    TipResolution tipResolution = TipResolution::AutoTip;
    std::string tipFileName; // the resolved tip file's name ("" unless ByMd5/ByFilename)

    // Lazy-icon coordinates: the source bundle and the preset's entry inside it.
    std::string sourcePath;
    std::string entryName;
};

// The scan's honesty counters, in the docio discipline: a bundle that lost content on the way
// in must not look intact.
struct LibraryCounters {
    int presetsLoaded = 0;
    int presetsFailed = 0;    // .kpp readKpp refused (version gate, broken container/XML)
    int tipsResolvedByMd5 = 0;
    int tipsResolvedByFilename = 0; // an md5 was claimed but matched nothing -- noted
    int tipsFallback = 0;           // reference matched nothing usable; default round tip
    int tipDecodeFailures = 0;      // a matched file that would not decode (also -> fallback)
    // The MASKING brush's own predefined tip references (§6.2), counted apart from the primary's
    // four counters above so their hand-derived census numbers keep meaning what they say.
    int maskingTipsResolved = 0;
    int maskingTipsFallback = 0; // unresolvable/undecodable: the analytic round mask, badged
    // The TEXTURE option's embedded patterns (§6.6h), counted apart for the same reason: a preset
    // whose grain silently failed to bake must not look intact.
    int texturesResolved = 0;
    int texturesFallback = 0; // the payload would not base64/PNG-decode: texturing off, badged
    int scanBudgetSkips = 0;  // brushes/ entries skipped by the extraction budget
};

// One extraction budget per addBundle scan: a hostile central directory can promise ~1000x
// amplification, and hashing is the only step that reads EVERY file. Generous -- Krita_4
// inflates to ~26 MB total.
inline constexpr std::uint64_t kMaxLibraryScanBytes = 512ull << 20;

class PresetLibrary {
public:
    // Scan one bundle held in memory; `sourcePath` is recorded for lazy icons and provenance.
    // Returns the number of presets added (0 with *error set when the container is unusable;
    // 0 with no error is a bundle that simply carries no presets).
    int addBundle(const std::uint8_t* data, std::size_t size, std::string_view sourcePath,
                  std::string* error = nullptr);

    // Convenience: read the file, then addBundle.
    int addBundleFile(const std::filesystem::path& path, std::string* error = nullptr);

    [[nodiscard]] const std::vector<LibraryPreset>& presets() const noexcept { return m_presets; }
    [[nodiscard]] const LibraryCounters& counters() const noexcept { return m_counters; }

    // The attribution of every source scanned so far (bundle meta.xml, §4.1: preserved, shown,
    // never used to inspect or restrict).
    struct Source {
        std::string path;
        BundleMeta meta;
    };
    [[nodiscard]] const std::vector<Source>& sources() const noexcept { return m_sources; }

    // Decode one preset's icon -- the .kpp's own raster (§3.1). Opens the source archive on the
    // first call for that source and keeps it open; the caller still caches what it SHOWS.
    [[nodiscard]] std::optional<common::Image> loadIcon(const LibraryPreset& preset,
                                                        std::string* error = nullptr) const;

    // How many source archives have been READ AND OPENED for icon fetches. It must not grow with
    // the number of icons -- one bundle, one open, however many presets are shown. This is the
    // whole point of the retained cache, and it is the only way to assert it that is not a stopwatch.
    [[nodiscard]] int archiveOpens() const noexcept { return m_archiveOpens; }
    // How many times loadIcon actually did the work. MONOTONIC -- it counts events, not residents,
    // and that is the entire point: a CACHE SIZE cannot witness a re-decode, because a re-decode
    // refills the cache to exactly the size it was. (It cost a surviving mutant to learn that. The
    // caller's cache is the thing under test; this is the instrument, and an instrument that resets
    // with the thing it measures is not one.)
    [[nodiscard]] int iconLoads() const noexcept { return m_iconLoads; }

private:
    // One source archive, opened lazily and retained. `bytes` lives behind a unique_ptr because
    // `zip` is a NON-OWNING view over it (zip.hpp): the buffer's address must outlive the reader
    // and must not move under it. A cache entry whose `zip` is empty is a remembered FAILURE --
    // an unreadable source is not retried on every frame.
    struct OpenSource {
        std::string path;
        std::vector<std::uint8_t> bytes;
        std::optional<ZipReader> zip;
    };
    // Mutable because loadIcon() is const and this is a pure memo: it changes what the call COSTS,
    // never what it returns. Not thread-safe -- loadIcon is a UI-thread call.
    [[nodiscard]] const ZipReader* openSource(const std::string& path, std::string* error) const;
    mutable std::vector<std::unique_ptr<OpenSource>> m_open;
    mutable int m_archiveOpens = 0; // counts the READS, not the cache hits
    mutable int m_iconLoads = 0;    // counts the CALLS; never reset, never decremented

    std::vector<LibraryPreset> m_presets;
    std::vector<Source> m_sources;
    LibraryCounters m_counters;
};

} // namespace mosaic::io::brush
