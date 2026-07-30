#include "core/texture/render_worker.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace mosaic::core::texture {

struct TextureRenderWorker::Impl {
    std::mutex mx;
    std::condition_variable cv;
    Job pending;
    bool hasJob = false;
    bool stop = false;
    Result result;
    bool hasResult = false;
    // The in-flight render's progress/cancel channel. Fresh per job (so a request() that cancels
    // the current render can never clip the NEXT one), shared so progressFraction() may keep
    // reading it while the worker moves on.
    std::shared_ptr<TextureRenderProgress> active;

    std::thread thread;

// GCC 16 at -O2 miscounts the make_shared control block's inline storage when the shared_ptr
// release chain is fully inlined ("std::mutex[0] partly outside array bounds of unsigned char
// [40]", via stl_construct.h) -- the -Warray-bounds/_Sp_counted_ptr_inplace false-positive
// family, not a real out-of-bounds. Scoped to run() only.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
    void run() {
        for (;;) {
            Job job;
            std::shared_ptr<TextureRenderProgress> prog;
            {
                std::unique_lock<std::mutex> lk(mx);
                cv.wait(lk, [this] { return hasJob || stop; });
                if (stop) return;
                job = std::move(pending);  // take the latest queued request (coalescing)
                hasJob = false;
                prog = std::make_shared<TextureRenderProgress>();
                active = prog;
            }
            TextureRenderResult render =
                renderTexture(job.params, job.frameW, job.frameH, job.window, prog.get());
            const bool cancelled = prog->cancel.load(std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(mx);
                if (active == prog) active.reset();
                if (!cancelled) {
                    result = Result{job.epoch, std::move(render)};
                    hasResult = true;  // supersedes any earlier untaken result
                }
            }
        }
    }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
};

TextureRenderWorker::TextureRenderWorker() : m_impl(std::make_unique<Impl>()) {
    // The Impl address is stable (held by unique_ptr), so the thread's raw pointer stays valid.
    Impl* p = m_impl.get();
    m_impl->thread = std::thread([p] { p->run(); });
}

TextureRenderWorker::~TextureRenderWorker() {
    if (!m_impl) return;
    {
        std::lock_guard<std::mutex> lk(m_impl->mx);
        m_impl->stop = true;
        if (m_impl->active) m_impl->active->cancel.store(true, std::memory_order_relaxed);
    }
    m_impl->cv.notify_one();
    if (m_impl->thread.joinable()) m_impl->thread.join();
}

void TextureRenderWorker::request(Job job) {
    {
        std::lock_guard<std::mutex> lk(m_impl->mx);
        m_impl->pending = std::move(job);
        m_impl->hasJob = true;
        // Abort the render in flight: its rows are already stale. Its own (per-job) channel, so
        // the request we just queued is untouched.
        if (m_impl->active) m_impl->active->cancel.store(true, std::memory_order_relaxed);
    }
    m_impl->cv.notify_one();
}

void TextureRenderWorker::cancelAll() {
    std::lock_guard<std::mutex> lk(m_impl->mx);
    m_impl->hasJob = false;
    m_impl->hasResult = false;
    if (m_impl->active) m_impl->active->cancel.store(true, std::memory_order_relaxed);
}

std::optional<TextureRenderWorker::Result> TextureRenderWorker::takeResult() {
    std::lock_guard<std::mutex> lk(m_impl->mx);
    if (!m_impl->hasResult) return std::nullopt;
    m_impl->hasResult = false;
    return std::move(m_impl->result);
}

double TextureRenderWorker::progressFraction() const {
    std::lock_guard<std::mutex> lk(m_impl->mx);
    if (!m_impl->active) return 0.0;
    const auto total = m_impl->active->rowsTotal.load(std::memory_order_relaxed);
    if (total == 0) return 0.0;
    const auto done = m_impl->active->rowsDone.load(std::memory_order_relaxed);
    return done >= total ? 1.0 : static_cast<double>(done) / static_cast<double>(total);
}

bool TextureRenderWorker::busy() const {
    std::lock_guard<std::mutex> lk(m_impl->mx);
    return m_impl->hasJob || m_impl->active != nullptr;
}

}  // namespace mosaic::core::texture
