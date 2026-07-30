#pragma once

#include "common/image.hpp"
#include "ui/tool.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Tool icon packs (S52). A pack is a FOLDER holding `mosaic_icon_pack.json` (identity + credits;
// the file's presence is what marks a folder as a pack) plus one icon per tool, named by the
// STABLE tool key below: `<key>.svg` (vector, the usual) or `<key>.png` (raster packs); when a
// pack ships both, the SVG wins -- it scales. Packs are TOOLS ONLY -- panel chrome, dialog stage
// icons and every other glyph are out of a pack's reach by design (PLAN §3.13's scope note); a
// pack can restyle what the eye picks from the toolbar and nothing else.
//
// The default pack ("Smalti", assets/default_tools/) is EMBEDDED in the binary like the other
// assets/ art, so it can never be missing; user packs are folders under dataDir()/icon_packs/.
// Resolution is PER ICON: a pack missing brush's icon gets the default brush, not a broken
// button -- a one-icon pack is a legitimate pack. Keys a pack ships beyond the known set are
// ignored.
//
// This module is FLTK-free (headless-tested). renderIconSource is the ONE pixels path -- SVG
// rasterized at the target size, PNG decoded and area-average scaled -- so vector and raster art
// look the same to every consumer (toolbar, flyout, settings cards), which keep their own pixel
// caches.
namespace mosaic::ui {

// The manifest's exact file name -- doubling as the "this folder is an icon pack" marker.
inline constexpr std::string_view kIconPackManifest = "mosaic_icon_pack.json";

// The embedded default pack's id ("Smalti" is its display name; ids are folder names).
inline constexpr std::string_view kDefaultIconPackId = "default";

// A pack may not exceed this many bytes per icon file (.svg or .png) -- hostile packs are
// third-party files, and a toolbar glyph has no business being megabytes.
inline constexpr std::size_t kMaxIconFileBytes = 256u * 1024;

// A raster icon's decoded edge cap: a 256 KB PNG can inflate to a gigapixel bomb; a glyph cannot.
inline constexpr std::uint32_t kMaxIconPngEdge = 2048;

// The stable file key for a tool ("brush" -> brush.svg). Every ToolId has one.
[[nodiscard]] std::string_view iconKeyFor(ToolId id) noexcept;

// The embedded default pack's SVG source for a tool -- the art every Tool is REGISTERED with
// (tool.cpp), so the default pack IS the baseline and no placeholder art exists beside it.
[[nodiscard]] std::string_view defaultIconSvg(ToolId id) noexcept;

// Every key a pack may carry, in display order: the implemented toolset first (matching
// iconKeyFor over today's ToolIds), then the tools PLAN already names (magic wand, selection
// brush, blur/dodge/burn/smudge, pen/path, heal, red eye, clone stamp, hand, warp) and the
// foreseeable magnetic lasso -- so a pack drawn today is not orphaned by the next tool session.
[[nodiscard]] const std::vector<std::string>& allIconKeys();

struct IconPackInfo {
    std::string id;          // folder name; kDefaultIconPackId for the embedded pack
    std::string name;        // display name from the manifest
    std::string artist;      // credit line
    std::string link;        // website / social handle / e-mail (one line, optional)
    std::string description; // one short paragraph (optional)
    std::string license;     // optional, shown nowhere yet but preserved
    bool builtin = false;
    std::filesystem::path dir; // empty for the embedded pack
};

class IconPacks {
public:
    IconPacks(); // the embedded default pack is always pack [0]

    // Scan `dir` for pack folders (any directory holding mosaic_icon_pack.json). Replaces the
    // packs of any previous scan (the built-in default always stays). Unreadable or manifest-less
    // folders are skipped and counted -- the return value is how many were REJECTED, so a caller
    // can log honesty ("3 packs, 1 rejected"). A missing `dir` is simply zero packs, not an error.
    int scan(const std::filesystem::path& dir);

    [[nodiscard]] const std::vector<IconPackInfo>& packs() const noexcept { return m_packs; }
    [[nodiscard]] const IconPackInfo* find(std::string_view id) const;

    // The icon source for `key` out of pack `packId` -- `<key>.svg` first, `<key>.png` second --
    // falling back PER ICON to the default pack when the pack is unknown, the files are
    // missing/unreadable/oversized, or the key is foreign. Empty only for a key the default pack
    // itself lacks. Loaded files are cached; rescanning drops the cache.
    [[nodiscard]] const IconSource& iconFor(std::string_view packId, std::string_view key);

    // Convenience over iconFor for the toolbar's consumers.
    [[nodiscard]] const IconSource& iconForTool(std::string_view packId, ToolId id) {
        return iconFor(packId, iconKeyFor(id));
    }

    // iconFor rendered at px -- the browser previews' one-stop call. Empty image on an
    // unrenderable source (renderIconSource's contract).
    [[nodiscard]] common::Image renderIcon(std::string_view packId, std::string_view key, int px);

private:
    std::vector<IconPackInfo> m_packs;
    std::map<std::string, IconSource, std::less<>> m_cache; // "<packId>/<key>" -> source
};

// Render an icon source at px x px: SVG rasterized (nanosvg) at the target size; PNG decoded,
// alpha-weighted area-average scaled, and letterboxed to the square (transparent margins) so a
// non-square raster still centers in the button. Empty image (+ reason) when the source is empty
// or undecodable -- consumers already warn-and-skip on that.
[[nodiscard]] common::Image renderIconSource(const IconSource& icon, int px,
                                             std::string* error = nullptr);

} // namespace mosaic::ui
