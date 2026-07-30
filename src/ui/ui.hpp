#pragma once

#include <string_view>

// ui -- the FLTK shell, custom widgets, tools, panels, dialogs, menus and theming. Real
// implementation begins in session S3; this is a placeholder identity.
namespace mosaic::ui {
std::string_view moduleName() noexcept;
}
