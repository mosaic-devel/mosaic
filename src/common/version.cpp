#include "common/version.hpp"

#include "version_config.hpp"

namespace mosaic::common {

std::string_view appName() noexcept { return MOSAIC_NAME; }

std::string_view appVersion() noexcept { return MOSAIC_VERSION_STRING; }

std::string_view appGitRev() noexcept { return MOSAIC_GIT_REV; }

std::string buildInfo() {
    const std::string buildType = std::string(MOSAIC_BUILD_TYPE).empty()
                                      ? std::string("Unknown")
                                      : std::string(MOSAIC_BUILD_TYPE);
    // Pre-1.0 (PLAN §8.9): append the short git commit as build metadata so each build -- including a
    // session's sub-commits, which share the 0.<phase>.<session> number -- is uniquely identifiable.
    std::string version = MOSAIC_VERSION_STRING;
    if (MOSAIC_GIT_REV[0] != '\0')
        version += std::string("+") + MOSAIC_GIT_REV;
    return std::string(MOSAIC_NAME) + ' ' + version + " (" + buildType + ", " + MOSAIC_COMPILER_ID +
           ' ' + MOSAIC_COMPILER_VERSION + ')';
}

}  // namespace mosaic::common
