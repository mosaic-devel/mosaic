#pragma once

// Scripting-shim inpaint backend for Mosaic (PLAN S37-b scaffold; wired by S40, §3.11).
//
// This backend does no inpainting itself. It forwards the request to a provider that a Lua script
// registers through the (future) S40 inpaint-hook API, then returns the provider's pixels. This is
// the entire mechanism by which user ML inpainting (LaMa, MAT, a diffusion model, …) reaches
// Mosaic — the user brings the dependency, weights, and runtime; Mosaic bundles none of it.
//
// Until S40 sets a provider it is inert: run() returns ok == false with an explanatory detail and
// the image unchanged.

#include "common/i18n.hpp" // N_(): mark for extraction (shown by the History panel)
#include "core/inpaint/inpaint_backend.hpp"

#include <functional>
#include <utility>

namespace mosaic::core::inpaint {

class ScriptBackend final : public IInpaintBackend {
public:
    // What S40 plugs in: pure pixels-in / pixels-out. Marshaling to/from Lua lives in the scripting
    // layer, not here, so core has no scripting dependency.
    using Provider = std::function<InpaintResult(const InpaintRequest&, const ProgressFn&)>;

    void setProvider(Provider provider) { m_provider = std::move(provider); }
    [[nodiscard]] bool hasProvider() const noexcept { return static_cast<bool>(m_provider); }

    [[nodiscard]] std::string id() const override { return "script"; }
    [[nodiscard]] std::string name() const override { return N_("Script provider"); }

    // Hidden from the engine selector until a Lua provider is registered (S40): an inert backend the
    // user can't run would only confuse the picker.
    [[nodiscard]] bool available() const override { return hasProvider(); }

    [[nodiscard]] BackendInfo info() const override {
        return BackendInfo{
            /*displayName*/ "Script provider",
            /*method*/ "User-supplied (Lua)",
            /*authors*/ "",
            /*paper*/ "",
            /*summary*/
            "Hands the region to a backend a Lua script registers through the inpainting API. "
            "Bring your own method (e.g. an ML model); Mosaic bundles none. Inert until a script "
            "is loaded (S40).",
            /*deviations*/ {},
            /*augmentations*/ {},
            /*cost*/ "Depends on the script."};
    }

    [[nodiscard]] InpaintResult run(const InpaintRequest& request,
                                    const ProgressFn& progress) override;

private:
    Provider m_provider;
};

}  // namespace mosaic::core::inpaint
