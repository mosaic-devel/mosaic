#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "core/texture/texture_params.hpp"
#include "core/texture/texture_render.hpp"

// Background texture renderer (S55-f; docs/texture-generator.md §7.4/§8.2): the dialog's live
// proxy AND its full-res Create bake run here, so a gizmo drag or a volumetric march never pins
// the UI thread. The SpellCheckWorker shape (the D1 background-thread precedent): ONE worker
// thread, requests COALESCE to the newest, each result is tagged with its request's EPOCH and the
// UI applies a result only when the epoch still matches. The texture twist: a render takes
// seconds, so a new request also CANCELS the render already in flight through the engine's
// TextureRenderProgress channel (it aborts between rows), and the same channel feeds the UI's
// progress bar. Poll from a re-armed FLTK timeout (the inpaint-job pattern); the worker never
// touches FLTK. FLTK-free and self-contained -- headless-testable.
namespace mosaic::core::texture {

class TextureRenderWorker {
public:
    TextureRenderWorker();   // spawns the worker thread
    ~TextureRenderWorker();  // cancels, signals + joins it
    TextureRenderWorker(const TextureRenderWorker&) = delete;
    TextureRenderWorker& operator=(const TextureRenderWorker&) = delete;
    TextureRenderWorker(TextureRenderWorker&&) = delete;
    TextureRenderWorker& operator=(TextureRenderWorker&&) = delete;

    struct Job {
        std::uint64_t epoch = 0;  // the caller's request generation (stale results are discarded)
        TextureParams params;
        std::uint32_t frameW = 0;  // the FULL frame the camera/deckle/LOD see
        std::uint32_t frameH = 0;
        TextureWindow window{};  // the sub-rect to evaluate (default = whole frame)
    };

    // Queue `job`, superseding any still-pending request and cancelling the render in flight.
    // Thread-safe; returns immediately.
    void request(Job job);

    // Cancel everything (pending + in flight) without queueing new work -- Create's Cancel
    // button and the dialog teardown.
    void cancelAll();

    struct Result {
        std::uint64_t epoch = 0;
        TextureRenderResult render;  // always complete (cancelled renders are never surfaced)
    };
    // The most recently COMPLETED render not yet taken, or nullopt. Non-blocking.
    [[nodiscard]] std::optional<Result> takeResult();

    // Live progress of the render in flight: completed-row fraction in [0, 1] (0 while the
    // engine has not yet published a row total), and whether any work is queued or running.
    [[nodiscard]] double progressFraction() const;
    [[nodiscard]] bool busy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::core::texture
