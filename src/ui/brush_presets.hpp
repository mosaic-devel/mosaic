#pragma once

#include "common/image.hpp"
#include "core/brush/brush_engine.hpp"
#include "io/brush/library.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The app's brush-preset store (docs/brushes.md §7, §8): scan the bundles Mosaic ships and any the
// user has installed, keep the library, and hold the ONE preset the Brush tool is painting with.
//
// The scan runs once at startup and reads two directories, in this order:
//   * installedDataDir()/brushes -- the shipped CC-0 set, read-only (data/brushes/)
//   * dataDir()/brushes          -- the user's own, which may SHADOW nothing: both are simply added
//
// `.bundle` files, plus -- since the editor landed (§8.3) -- LOOSE `.mbp` and `.kpp` files in the
// same two directories. A loose file has no manifest to attribute it, and §4.1's posture is that
// imported content is preserved and shown, never inspected; so a loose preset carries the
// attribution its own container holds and nothing is inferred from the file name.
//
// ⚠ A LOOSE PRESET IS RESOLVED BY ADOPTION, NOT BY RE-DERIVATION (see adoptLoosePreset). A bundle
// carries the tip files and the texture payloads its presets reference, and `PresetLibrary` decodes
// and BAKES them on the way in. A loose file references the same resources and ships none of them,
// so the only honest resolution is to reuse a build the library already made for the identical
// resource -- never to run a second copy of the library's baking rules here, which would be two
// spellings of one rule and would drift.
//
// ⚠ A PRESET IS RESOLVED ONCE, WHEN IT IS SELECTED -- never per stroke. `presetBrushParams` mints a
// fresh raster id for the tip, and a fresh id is a cold dab cache (io/brush/preset_brush.hpp). The
// store is what makes that a rule rather than a hope: `activeParams()` hands out a pointer to the
// resolved params, and the canvas copies from it per stroke.
//
// FLTK-free, so it is unit-tested headlessly.
namespace mosaic::ui {

class BrushPresetStore {
public:
    // Scan both directories. Idempotent-ish: it appends, so call it once. Returns the number of
    // presets loaded (0 is not an error -- a build with no data dir simply has no presets, and the
    // Brush falls back to its plain round tip, which is what it has always painted with).
    int scan();

    // Scan one directory: its `.bundle` files first (the library's), then its loose `.mbp` / `.kpp`
    // files. Exposed for the tests, which point it at a fixture. ⚠ It draws NO shipped|user
    // boundary -- only scan() does, and isUserPreset's -1 guard is what makes a bare scanDir()
    // report nothing as the user's rather than everything.
    int scanDir(const std::filesystem::path& dir);

    [[nodiscard]] const io::brush::PresetLibrary& library() const noexcept { return m_lib; }
    // The whole corpus: THE USER'S OWN FIRST, then everything Mosaic ships. Within each half the
    // library's bundles come before the loose files, and each of those keeps its scan order.
    //
    // ⚠ THE USER'S RUN LEADS, AND THAT IS THE USER'S CALL (feedback round 1, 2026-07-29): a brush
    // you made yourself is the one you are reaching for, and it used to be appended after 114 you
    // did not. It also makes the ordering rule ONE rule rather than an accident of scan order --
    // `presets()` is the only place it is spelled, and `isUserPreset` reads the SAME arithmetic
    // rather than a second copy of it (m_userCount).
    //
    // When nothing loose is installed and no user bundle was scanned -- which is every fresh
    // install -- this IS the library's own vector and costs nothing; the merged copy is built only
    // once there is something to merge or to reorder (see m_all).
    [[nodiscard]] const std::vector<io::brush::LibraryPreset>& presets() const noexcept {
        return m_all.empty() ? m_lib.presets() : m_all;
    }
    [[nodiscard]] std::vector<std::string> names() const;

    // Did this preset come from the USER's data dir rather than the shipped bundle? The dock's "User"
    // tab exists only while at least one does -- a tab advertising an empty room is worse than no tab.
    //
    // It is an INDEX RANGE, not a path comparison, and that follows from scan()'s order: the shipped
    // directory is read first and the user's second, so everything at or past the high-water mark of
    // the first scan is theirs. A path comparison would have to re-derive installedDataDir() here and
    // agree with it forever; the boundary is simply recorded where it is drawn.
    //
    // ⚠ -1 means NO BOUNDARY WAS EVER DRAWN -- which is the case for a bare scanDir() (what the tests
    // and any future importer do). Then nothing is a user preset, rather than EVERYTHING being one,
    // which is what an unguarded `index >= 0` would have said. The two splits below still carry that
    // -1, and they are what m_userCount is COUNTED from, so the guard survives the reordering.
    // ⚠ AND THE CORPUS IS TWO RUNS, so the boundary is two numbers (m_libUserSplit /
    // m_looseUserSplit): a single index would have to assume the user installed nothing loose into
    // the SHIPPED directory, which is an assumption and not a fact.
    //
    // ⚠ SINCE THE USER'S RUN LEADS (see presets()), the range is a PREFIX -- `[0, m_userCount)` --
    // rather than a suffix. It is still an index range and not a path comparison, for the reason it
    // always was: a path comparison would have to re-derive installedDataDir() here and agree with
    // it forever, where the boundary is simply recorded where it is drawn.
    [[nodiscard]] bool isUserPreset(int index) const noexcept {
        return index >= 0 && index < m_userCount;
    }
    [[nodiscard]] int userPresetCount() const noexcept { return m_userCount; }

    // The index of the first preset named `name`, or -1 for no match (an empty name included). The
    // SELECTION is persisted by name, never by index (common::Settings::brushPreset): the library's
    // order is a directory scan, so an index would silently point at a different brush the moment a
    // bundle is added or renamed. Names are not unique across bundles -- first match wins, which is
    // the same preset the sorted scan showed the user.
    [[nodiscard]] int indexOfName(std::string_view name) const;

    // TWO SELECTIONS, ONE LIBRARY. The Brush and the Eraser each hold their own preset, because they
    // are different tools reaching for different corpora -- a preset carrying `CompositeOp=erase` is
    // an eraser (the mapper turns it into StrokeMode::Erase), and the dock shows the Brush every
    // preset that is NOT one and the Eraser nothing but. A single slot would mean switching to the
    // Eraser silently changed which brush you were painting with, and back again.
    //
    // Select by index into presets(), or -1 for NO preset: the engine's built-in analytic circle,
    // which is exactly what both tools painted before presets existed and stays the default for
    // either. Resolves the params once. Out-of-range selects nothing and returns false.
    bool select(int index);
    bool selectEraser(int index);
    [[nodiscard]] int activeIndex() const noexcept { return m_brush.index; }
    [[nodiscard]] int activeEraserIndex() const noexcept { return m_eraser.index; }

    // The active preset's params, or NULL when none is selected. The canvas copies from this and
    // overlays the context bar's live values (size, opacity) and the editor's colour on top.
    [[nodiscard]] const core::brush::BrushParams* activeParams() const noexcept {
        return m_brush.params ? m_brush.params.get() : nullptr;
    }
    [[nodiscard]] const core::brush::BrushParams* activeEraserParams() const noexcept {
        return m_eraser.params ? m_eraser.params.get() : nullptr;
    }
    [[nodiscard]] const io::brush::LibraryPreset* activePreset() const noexcept;
    [[nodiscard]] const io::brush::LibraryPreset* activeEraserPreset() const noexcept;
    [[nodiscard]] const io::brush::LibraryPreset* presetAt(int index) const noexcept;

    // ---- The editor's write side (docs/brushes.md §8.3) ---------------------------------------

    // `wanted`, made unique against every name the corpus already holds, by appending " copy",
    // " copy 2", ... A brush that shadowed a shipped one by name would make `indexOfName` -- and
    // therefore the persisted `Settings::brushPreset` -- point at whichever the scan happened to
    // reach first, which is a coin toss between the user's edit and the file they edited it from.
    // Pure given the corpus; the free function below is the testable half.
    [[nodiscard]] std::string uniqueName(std::string_view wanted) const;

    // Write `lp` to `dataDir()/brushes/<stem>.mbp` and take it into the store, with `icon` as the
    // container's raster (which IS the preset's thumbnail, §3.1 -- so the editor hands in a render
    // of the brush's own stroke).
    //
    // `replaceIndex` >= 0 overwrites that preset -- which must be one of the user's own LOOSE ones;
    // a shipped bundle is read-only and passing an index inside one fails rather than silently
    // creating a second brush of the same name. -1 creates a new preset.
    //
    // Returns the preset's index in `presets()`, or -1 with *error set. The store keeps the
    // RESOLVED `lp` (tip, masking, texture and all), so the brush you just saved paints exactly what
    // the editor previewed for the rest of the session; the file on disk carries the references,
    // which the next launch adopts (see adoptLoosePreset).
    int writeUserPreset(const io::brush::LibraryPreset& lp, const common::Image& icon,
                        int replaceIndex, std::string* error);

    // Take a preset file the user chose -- `.mbp`, `.kpp` or `.bundle` -- into `dataDir()/brushes`
    // and into the store. Returns the index of the FIRST preset it added, or -1 with *error set.
    // A `.bundle` may add many; only the first is reported, because that is the one to select.
    int importPresetFile(const std::filesystem::path& path, std::string* error);

    // ---- Delete (docs/brushes.md §8.3, feedback round 1) ---------------------------------------

    // Can `index` be deleted at all? True ONLY for one of the user's own LOOSE files.
    //
    // ⚠ TWO REFUSALS, NOT ONE. A shipped preset is read-only for the same reason `Save` never
    // overwrites one -- Mosaic must not edit the set it ships. But a preset inside a user-installed
    // `.bundle` is refused TOO: it has no file of its own, and the only thing "delete" could mean
    // there is "remove the archive and the other 40 presets in it", which is not what the button
    // says. `deleteRefusal` is the sentence for the one that was refused.
    [[nodiscard]] bool canDeletePreset(int index) const noexcept;
    [[nodiscard]] std::string deleteRefusal(int index) const;

    // Remove one of the user's own loose presets: the FILE in `dataDir()/brushes`, and the entry.
    //
    // ⚠ This is a WRITE to the user's data directory, so it happens only on an explicit, confirmed
    // user action -- the editor asks first (§8.3). It refuses anything canDeletePreset() refuses.
    //
    // ⚠ A SELECTION POINTING AT THE DELETED BRUSH FALLS BACK TO NO PRESET -- index -1, the engine's
    // own analytic circle, which is the one selection that is always valid. Both the name AND the
    // resolved params are dropped with it: an index that read -1 while activeParams() still handed
    // out the deleted preset's tip would keep painting with a brush that no longer exists.
    //
    // Returns true, or false with *error set. `*nextIndex` (optional) receives the index the UI
    // should now point at -- the preset that took the deleted one's place, or -1.
    bool deleteUserPreset(int index, std::string* error, int* nextIndex = nullptr);

private:
    struct Selection {
        int index = -1;
        // ⚠ The NAME the index was resolved from, kept beside it. Adding a bundle at runtime shifts
        // every loose preset's index, and rebuildAll() re-points from this rather than from the
        // index it is about to invalidate -- the same reason the SETTINGS store the selection by
        // name (§8.2). Empty means "no preset": the engine's own analytic circle.
        std::string name;
        std::shared_ptr<const core::brush::BrushParams> params;
    };
    bool selectInto(Selection& slot, int index);

    // Scan `dir` for loose `.mbp` / `.kpp` files (the `.bundle`s are the library's). Returns how
    // many presets were added.
    int scanLooseDir(const std::filesystem::path& dir);
    // Turn a parsed preset + the file it came from into a LibraryPreset by ADOPTING the library's
    // already-resolved resources (see the header note): the bitmap tip built for the same tip file
    // name, and the texture pattern baked from the same embedded payload. Anything that finds no
    // donor degrades exactly as the library degrades an unresolvable bundle reference -- the round
    // tip / no texturing, badged in `provenance.droppedOptions` -- so a preset never disappears.
    [[nodiscard]] io::brush::LibraryPreset adoptLoosePreset(io::brush::BrushPreset preset,
                                                            const std::filesystem::path& path) const;
    // Rebuild the merged view after either run changed, and re-point both selections BY NAME --
    // adding a bundle at runtime shifts every loose preset's index, and an index that silently
    // re-points is the bug the persisted selection is stored by name to avoid.
    void rebuildAll();

    // How many of each run are SHIPPED (the count before the user boundary). -1 = no boundary was
    // drawn, which means the whole run is shipped -- never that the whole run is the user's.
    [[nodiscard]] int shippedLibCount() const noexcept;
    [[nodiscard]] int shippedLooseCount() const noexcept;
    // Where a slot of either run lands in the merged, user-first view. The ONE place the ordering
    // is inverted; every caller that has just written into m_lib or m_loose goes through it rather
    // than re-deriving an index the reorder would make wrong.
    [[nodiscard]] int mergedIndexOfLib(int libSlot) const noexcept;
    [[nodiscard]] int mergedIndexOfLoose(int looseSlot) const noexcept;
    // ... and back: the m_loose slot a merged index names, or -1 when it names a library preset
    // (which has no loose file, and so can be neither overwritten in place nor deleted).
    [[nodiscard]] int looseSlotOf(int index) const noexcept;

    io::brush::PresetLibrary m_lib;
    std::vector<io::brush::LibraryPreset> m_loose; // loose `.mbp` / `.kpp` files, in scan order
    // The merged, USER-FIRST view. Empty when the corpus needs neither merging nor reordering --
    // i.e. no loose file and no user bundle -- in which case presets() hands out m_lib's own vector.
    std::vector<io::brush::LibraryPreset> m_all;
    int m_libUserSplit = -1;   // first USER preset within m_lib.presets(); -1 = no user scan ran
    int m_looseUserSplit = -1; // ... and within m_loose. See isUserPreset.
    int m_userCount = 0;       // how many of presets() are the user's -- always the FIRST this many
    Selection m_brush;
    Selection m_eraser;
};

// The unique-name rule, as a pure function of the names already taken (BrushPresetStore::uniqueName).
// `wanted` when nothing holds it; otherwise "wanted copy", "wanted copy 2", ... An empty `wanted`
// falls back to a generic name rather than producing a preset that `indexOfName` can never find --
// "" is how the settings spell NO PRESET.
[[nodiscard]] std::string uniquePresetName(const std::vector<std::string>& taken,
                                           std::string_view wanted);

// A preset name reduced to a portable file stem: ASCII alphanumerics and '-' kept, everything else
// (spaces, ')', '/', accents, every byte a filesystem or a zip might argue about) collapsed to '_',
// capped, and never empty. It is a FILE NAME and not an identity -- the preset's real name lives in
// the container -- so two presets whose names collapse to the same stem are disambiguated by a
// numeric suffix at write time rather than by making this injective.
[[nodiscard]] std::string presetFileStem(std::string_view name);

// The picture a saved preset carries when its caller has none: a square render of the brush's own
// stroke. The size matches the `.kpp` raster the shipped set ships (200 px), and the diameter
// ceiling is what keeps a 1000 px nib from filling the square with one solid blob -- the dock's
// card rule, at a card's own scale (core/brush/stroke_preview.hpp).
inline constexpr int kUserPresetIconPx = 200;
inline constexpr double kUserPresetIconDiameter = 96.0;

// Serialize `lp` into Mosaic's own `.mbp` container and write it to `path`, with `icon` as the
// container's raster -- or, when `icon` is empty, a fresh render of the brush's own stroke, because
// the raster IS the preset's thumbnail (§3.1) and `writeMbp` refuses an empty one.
//
// ⚠ ONE WRITER, TWO CALLERS. `writeUserPreset` writes into `dataDir()/brushes`; EXPORT writes
// wherever the user pointed a save dialog. They must produce the identical bytes -- an export that
// went through a second serializer would drift from the file the app itself reads back.
//
// ⚠ MOSAIC DOES NOT WRITE `.kpp`, and export does not change that (§8.3 ②). Round-tripping one means
// a PNG-chunk writer plus an XML serializer for 94 params whose defaults, spellings and two
// prefixing rules all have to be reproduced exactly or the file loads differently somewhere else.
[[nodiscard]] bool writePresetFile(const io::brush::LibraryPreset& lp, const common::Image& icon,
                                   const std::filesystem::path& path, std::string* error);

// `path` with `.mbp` appended unless it already ends in it (case-insensitively). A save dialog can
// come back with a bare name -- the portal does not append the filter's extension -- and a preset
// file that is not named `.mbp` is one the next scan will not look at.
[[nodiscard]] std::filesystem::path withMbpExtension(std::filesystem::path path);

} // namespace mosaic::ui
