#pragma once

#include <cstddef>
#include <string>
#include <vector>

// The recent-files ring buffer behind File -> Open Recent and the New Document dialog's Recent
// cards (S55). Pure list operations over Settings::recentFiles -- newest first, deduplicated,
// capped -- so the policy is unit-tested headlessly; callers canonicalize paths before pushing
// (one spelling per file) and persist via Settings.
namespace mosaic::common {

// Capacity of the recent-files list. Deliberately menu-sized: every entry is a row in the
// File -> Open Recent pop-up, so the cap keeps the menu (and the dialog's Recent rail) short.
inline constexpr std::size_t kMaxRecentFiles = 10;

// Capacity of the New Document dialog's remembered custom sizes ("last few", user round 5) --
// one gallery row's worth, so the Sizes section never crowds out the files below it.
inline constexpr std::size_t kMaxRecentSizes = 5;

// Move-or-insert `path` at the front: an existing entry (exact string match) is moved rather
// than duplicated, and the list is trimmed to `cap`. An empty `path` is ignored.
void pushRecentFile(std::vector<std::string>& recents, const std::string& path,
                    std::size_t cap = kMaxRecentFiles);

// Drop `path` (exact string match) wherever it sits; keeps order otherwise. Used when opening
// a recent entry fails (the file has moved or vanished) so the list self-heals.
void removeRecentFile(std::vector<std::string>& recents, const std::string& path);

} // namespace mosaic::common
