#pragma once

// Inpainting engine for Mosaic (PLAN S37-b, §3.11): a small registry + dispatcher over
// IInpaintBackend. Every inpaint entry point — Heal tool (S38), Inpaint brush + Edit→Fill→Inpaint
// (S39) — calls InpaintEngine::run(); choosing an algorithm is a backend selection, not a rewrite.
// Built-in backends register at startup; the Lua ScriptBackend (S40) registers at script-load.

#include "core/inpaint/inpaint_backend.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mosaic::core::inpaint {

class InpaintEngine {
public:
    // Adds a backend. The first one registered becomes active if none is set yet. A backend whose
    // id() duplicates an existing one replaces it (so a Lua re-register updates in place).
    void registerBackend(std::shared_ptr<IInpaintBackend> backend);

    // Ids of all registered backends, in registration order.
    [[nodiscard]] std::vector<std::string> backendIds() const;

    // Selects the active backend by id. Returns false (and leaves the selection unchanged) if no
    // backend with that id is registered.
    bool setActiveBackend(std::string id);

    // The active backend id, or "" if none.
    [[nodiscard]] std::string activeBackend() const { return m_activeId; }

    // The registered backend with this id, or nullptr. Borrowed; owned by the engine.
    [[nodiscard]] const IInpaintBackend* backend(const std::string& id) const;

    [[nodiscard]] bool empty() const noexcept { return m_backends.empty(); }

    // Runs the active backend. With no active backend, returns ok == false and a copy of the input
    // image (callers always get a valid image back).
    [[nodiscard]] InpaintResult run(const InpaintRequest& request,
                                    const ProgressFn& progress = {}) const;

    // The active backend's analysed region for a hole (its sample neighbourhood), or nullopt when
    // there's no active backend or it reports none. Drives the UI's sample-area preview (S39)
    // without the app reaching into any backend's internals.
    [[nodiscard]] std::optional<common::Rect>
    analysedRegion(std::uint32_t imageW, std::uint32_t imageH,
                   const std::optional<common::Rect>& holeBounds, const Params& params) const;

private:
    std::vector<std::shared_ptr<IInpaintBackend>> m_backends;
    std::string                                   m_activeId;
};

// An engine with Mosaic's built-in backends registered. Active backend defaults to the cleared
// diffusion PdeBackend until the He & Sun OffsetStatisticsBackend lands (S37-c), at which point
// that becomes the default. (The inert ScriptBackend is registered too; S40 wires its provider.)
[[nodiscard]] InpaintEngine makeDefaultEngine();

}  // namespace mosaic::core::inpaint
