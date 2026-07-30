#include "core/texture/sky_estimate_worker.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace mosaic::core::texture {

struct SkyEstimateWorker::Impl {
    std::mutex mx;
    std::condition_variable cv;
    Job pending;
    bool hasJob = false;
    bool stop = false;
    Result result;
    bool hasResult = false;
    // The in-flight estimate's progress/cancel channel. Fresh per job (so a request() that
    // cancels the current run can never clip the NEXT one), shared so progressFraction() may
    // keep reading it while the worker moves on.
    std::shared_ptr<SkyEstimateProgress> active;

    std::thread thread;

// GCC 16 at -O2 miscounts the make_shared control block's inline storage when the shared_ptr
// release chain is fully inlined -- the -Warray-bounds/_Sp_counted_ptr_inplace false-positive
// family the render worker documents. Scoped to run() only.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
    void run() {
        for (;;) {
            Job job;
            std::shared_ptr<SkyEstimateProgress> prog;
            {
                std::unique_lock<std::mutex> lk(mx);
                cv.wait(lk, [this] { return hasJob || stop; });
                if (stop) return;
                job = std::move(pending);  // take the latest queued request (coalescing)
                hasJob = false;
                prog = std::make_shared<SkyEstimateProgress>();
                active = prog;
            }
            SkyEstimateResult estimate = estimateSkyFromLayer(job.photo, job.options, prog.get());
            const bool cancelled =
                estimate.cancelled || prog->cancel.load(std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(mx);
                if (active == prog) active.reset();
                if (!cancelled) {
                    result = Result{job.epoch, std::move(estimate)};
                    hasResult = true;  // supersedes any earlier untaken result
                }
            }
        }
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
};

SkyEstimateWorker::SkyEstimateWorker() : m_impl(std::make_unique<Impl>()) {
    // The Impl address is stable (held by unique_ptr), so the thread's raw pointer stays valid.
    Impl* p = m_impl.get();
    m_impl->thread = std::thread([p] { p->run(); });
}

SkyEstimateWorker::~SkyEstimateWorker() {
    if (!m_impl) return;
    {
        std::lock_guard<std::mutex> lk(m_impl->mx);
        m_impl->stop = true;
        if (m_impl->active) m_impl->active->cancel.store(true, std::memory_order_relaxed);
    }
    m_impl->cv.notify_one();
    if (m_impl->thread.joinable()) m_impl->thread.join();
}

void SkyEstimateWorker::request(Job job) {
    {
        std::lock_guard<std::mutex> lk(m_impl->mx);
        m_impl->pending = std::move(job);
        m_impl->hasJob = true;
        // Abort the estimate in flight: its stages are already stale. Its own (per-job)
        // channel, so the request we just queued is untouched.
        if (m_impl->active) m_impl->active->cancel.store(true, std::memory_order_relaxed);
    }
    m_impl->cv.notify_one();
}

void SkyEstimateWorker::cancelAll() {
    std::lock_guard<std::mutex> lk(m_impl->mx);
    m_impl->hasJob = false;
    m_impl->hasResult = false;
    if (m_impl->active) m_impl->active->cancel.store(true, std::memory_order_relaxed);
}

std::optional<SkyEstimateWorker::Result> SkyEstimateWorker::takeResult() {
    std::lock_guard<std::mutex> lk(m_impl->mx);
    if (!m_impl->hasResult) return std::nullopt;
    m_impl->hasResult = false;
    return std::move(m_impl->result);
}

double SkyEstimateWorker::progressFraction() const {
    std::lock_guard<std::mutex> lk(m_impl->mx);
    if (!m_impl->active) return 0.0;
    return m_impl->active->permille.load(std::memory_order_relaxed) / 1000.0;
}

bool SkyEstimateWorker::busy() const {
    std::lock_guard<std::mutex> lk(m_impl->mx);
    return m_impl->hasJob || m_impl->active != nullptr;
}

}  // namespace mosaic::core::texture
