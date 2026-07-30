#pragma once

#include <string_view>

// core -- the document model: layers, groups, masks, effects, undo/redo, color state.
// Real implementation begins in session S6; this is a placeholder identity.
namespace mosaic::core {
std::string_view moduleName() noexcept;
}
