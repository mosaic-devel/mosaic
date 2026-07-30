#pragma once

#include "common/image.hpp"

#include <filesystem>
#include <optional>
#include <string>

// Read-only lookup into the freedesktop.org shared thumbnail cache
// ($XDG_CACHE_HOME/thumbnails/{large,x-large,normal}/<md5 of the file URI>.png). The New
// Document dialog's plain-image recents show whatever preview the user's file managers already
// made; .mosaic files carry their own PRVW chunk (io/mosaic/preview.hpp). Mosaic deliberately
// WRITES no thumbnail cache of its own -- the app-owned thumbnails folder died with S48-b
// (user 2026-07-22): derived copies of the user's images are not ours to keep.
//
// The pure key helpers are headless-tested; only the load touches the filesystem.
namespace mosaic::ui {

// "file://" + the absolute path percent-encoded per RFC 3986, keeping the path-legal set
// (unreserved + sub-delims + ":@/") -- byte-compatible with GLib's g_filename_to_uri and
// QUrl::fromLocalFile for the paths both desktops feed the cache.
[[nodiscard]] std::string fileUriFor(const std::string& absolutePath);

// The cache file a URI keys inside one size bucket ("large", ...): <md5(uri)>.png.
[[nodiscard]] std::string xdgThumbnailName(const std::string& fileUri);

// $XDG_CACHE_HOME/thumbnails (or ~/.cache/thumbnails); empty when no cache root resolves.
[[nodiscard]] std::filesystem::path xdgThumbnailRoot();

// The freshest usable cached thumbnail for `absolutePath`, largest bucket first, or nullopt.
// An entry older than the source file is treated as stale and skipped (the spec's Thumb::MTime
// tag would be exact; mtime ordering is the honest approximation without a tEXt parser).
[[nodiscard]] std::optional<common::Image> loadXdgThumbnail(const std::string& absolutePath);

} // namespace mosaic::ui
