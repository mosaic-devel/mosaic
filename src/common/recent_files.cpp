#include "common/recent_files.hpp"

#include <algorithm>

namespace mosaic::common {

void pushRecentFile(std::vector<std::string>& recents, const std::string& path, std::size_t cap) {
    if (path.empty() || cap == 0)
        return;
    std::erase(recents, path);
    recents.insert(recents.begin(), path);
    if (recents.size() > cap)
        recents.resize(cap);
}

void removeRecentFile(std::vector<std::string>& recents, const std::string& path) {
    std::erase(recents, path);
}

} // namespace mosaic::common
