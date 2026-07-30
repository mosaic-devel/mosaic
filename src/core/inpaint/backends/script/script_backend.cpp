#include "core/inpaint/backends/script/script_backend.hpp"

namespace mosaic::core::inpaint {

InpaintResult ScriptBackend::run(const InpaintRequest& request, const ProgressFn& progress) {
    if (!m_provider) {
        return {request.image, false,
                "no script inpaint provider registered (a Lua script registers one in S40)"};
    }
    return m_provider(request, progress);
}

}  // namespace mosaic::core::inpaint
