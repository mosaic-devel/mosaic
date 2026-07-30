#pragma once

#include <string_view>

// platform -- OS integration: native window handles, DPI, drag & drop, system theme
// detection, tablet input. Real implementation begins in session S3; placeholder for now.
namespace mosaic::platform {
std::string_view moduleName() noexcept;
}
