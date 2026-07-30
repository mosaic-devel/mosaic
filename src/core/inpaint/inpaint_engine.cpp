#include "core/inpaint/inpaint_engine.hpp"

#include "core/inpaint/backends/he_sun/offset_stats_backend.hpp"
#include "core/inpaint/backends/pde/pde_backend.hpp"
#include "core/inpaint/backends/resynth/resynth_backend.hpp"
#include "core/inpaint/backends/script/script_backend.hpp"

#include <algorithm>
#include <utility>

namespace mosaic::core::inpaint {

void InpaintEngine::registerBackend(std::shared_ptr<IInpaintBackend> backend) {
    if (!backend) {
        return;
    }
    const std::string id = backend->id();
    const auto it = std::find_if(m_backends.begin(), m_backends.end(),
                                 [&](const auto& b) { return b->id() == id; });
    if (it != m_backends.end()) {
        *it = std::move(backend);  // re-register in place (e.g. a Lua script reloads its provider)
    } else {
        m_backends.push_back(std::move(backend));
    }
    if (m_activeId.empty()) {
        m_activeId = id;
    }
}

std::vector<std::string> InpaintEngine::backendIds() const {
    std::vector<std::string> ids;
    ids.reserve(m_backends.size());
    for (const auto& b : m_backends) {
        ids.push_back(b->id());
    }
    return ids;
}

const IInpaintBackend* InpaintEngine::backend(const std::string& id) const {
    const auto it = std::find_if(m_backends.begin(), m_backends.end(),
                                 [&](const auto& b) { return b->id() == id; });
    return it == m_backends.end() ? nullptr : it->get();
}

std::optional<common::Rect>
InpaintEngine::analysedRegion(std::uint32_t imageW, std::uint32_t imageH,
                              const std::optional<common::Rect>& holeBounds,
                              const Params& params) const {
    const IInpaintBackend* b = backend(m_activeId);
    return b != nullptr ? b->analysedRegion(imageW, imageH, holeBounds, params) : std::nullopt;
}

bool InpaintEngine::setActiveBackend(std::string id) {
    const auto it = std::find_if(m_backends.begin(), m_backends.end(),
                                 [&](const auto& b) { return b->id() == id; });
    if (it == m_backends.end()) {
        return false;
    }
    m_activeId = std::move(id);
    return true;
}

InpaintResult InpaintEngine::run(const InpaintRequest& request, const ProgressFn& progress) const {
    const auto it = std::find_if(m_backends.begin(), m_backends.end(),
                                 [&](const auto& b) { return b->id() == m_activeId; });
    if (it == m_backends.end()) {
        return {request.image, false, "no active inpaint backend"};
    }
    return (*it)->run(request, progress);
}

Params paramsForPreset(const IInpaintBackend& backend, const std::string& presetId) {
    Params params{};
    const BackendSettingsSchema schema = backend.settingsSchema();
    const PresetSpec* preset = nullptr;
    for (const auto& ps : schema.presets) {
        if (ps.id == presetId) {
            preset = &ps;
            break;
        }
    }
    for (const auto& control : schema.controls) {
        double value = control.defaultValue;
        if (preset != nullptr) {
            for (const auto& [key, presetValue] : preset->values) {
                if (key == control.key) {
                    value = presetValue;
                    break;
                }
            }
        }
        backend.applyParam(params, control.key, value);
    }
    return params;
}

InpaintEngine makeDefaultEngine() {
    InpaintEngine engine;
    engine.registerBackend(std::make_shared<PdeBackend>());
    engine.registerBackend(std::make_shared<OffsetStatisticsBackend>());
    engine.registerBackend(std::make_shared<ResynthBackend>());
    engine.registerBackend(std::make_shared<ScriptBackend>());
    // Default is the He & Sun offset-statistics solver — the higher-quality object-removal path,
    // now with a working-region crop + KD-tree NNF so its cost scales with the hole, not the image.
    // PdeBackend (cleared diffusion) stays available as a fast fallback for tiny scratches.
    engine.setActiveBackend("offset-stats");
    return engine;
}

}  // namespace mosaic::core::inpaint
