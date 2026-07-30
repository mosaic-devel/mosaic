#pragma once

#include <string>
#include <string_view>

namespace mosaic::common {

// Application display name (e.g. "Mosaic").
std::string_view appName() noexcept;

// Semantic version string (e.g. "0.1.0").
std::string_view appVersion() noexcept;

// Short git revision of the build ("g<hash>" or "g<hash>-dirty"), or empty when built outside a
// git checkout. This is the "+<rev>" build-metadata suffix buildInfo() appends and the About
// dialog shows muted next to the version.
std::string_view appGitRev() noexcept;

// One-line, human-readable build description (name, version, build type, compiler).
std::string buildInfo();

}  // namespace mosaic::common
