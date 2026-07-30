#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "common/image.hpp"
#include "core/texture/sky_estimate.hpp"

// Background "Estimate from layer" runner (S55; docs/research-sky-estimate-from-layer.md §7):
// the TextureRenderWorker shape verbatim -- ONE worker thread, requests COALESCE to the newest,
// results are epoch-tagged so the UI applies only a still-current one, and a new request cancels
// the estimate in flight through its SkyEstimateProgress channel (the engine checks between
// stages and between probe renders). progressFraction() feeds the dialog's footer bar through
// the same re-armed-timeout polling the bake uses. FLTK-free and self-contained --
// headless-testable. The estimate stage's probe signatures depend on renderTexture, which is
// pure, so running it here is exactly as safe as the render worker's own use.
namespace mosaic::core::texture {

class SkyEstimateWorker {
public:
    SkyEstimateWorker();   // spawns the worker thread
    ~SkyEstimateWorker();  // cancels, signals + joins it
    SkyEstimateWorker(const SkyEstimateWorker&) = delete;
    SkyEstimateWorker& operator=(const SkyEstimateWorker&) = delete;
    SkyEstimateWorker(SkyEstimateWorker&&) = delete;
    SkyEstimateWorker& operator=(SkyEstimateWorker&&) = delete;

    struct Job {
        std::uint64_t epoch = 0;  // the caller's request generation (stale results are discarded)
        common::Image photo;      // the doc-space layer image (activeLayerDocImage's product)
        SkyEstimateOptions options;
    };

    // Queue `job`, superseding any still-pending request and cancelling the estimate in flight.
    // Thread-safe; returns immediately.
    void request(Job job);

    // Cancel everything (pending + in flight) without queueing new work -- dialog teardown and
    // the footer's Cancel.
    void cancelAll();

    struct Result {
        std::uint64_t epoch = 0;
        SkyEstimateResult estimate;  // never a cancelled one (those are discarded here)
    };
    // The most recently COMPLETED estimate not yet taken, or nullopt. Non-blocking.
    [[nodiscard]] std::optional<Result> takeResult();

    // Live progress of the estimate in flight: the engine's fixed stage fractions in [0, 1],
    // and whether any work is queued or running.
    [[nodiscard]] double progressFraction() const;
    [[nodiscard]] bool busy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace mosaic::core::texture
