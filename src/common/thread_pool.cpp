#include "common/thread_pool.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace mosaic::common {
namespace {

// One submitted parallel loop. It lives on the SUBMITTING THREAD'S STACK for exactly as long as
// the call: `fn`/`ctx`/`bands` are written before the job is published and only read after, every
// other field is guarded by the pool mutex, and the submitter does not return until `remaining`
// reaches 0 -- so no worker can ever touch a job whose storage has died.
struct Job {
    detail::BandFn fn = nullptr;
    void* ctx = nullptr;
    std::size_t bands = 0;
    std::size_t claimed = 0;   // lowest band nobody has taken yet
    std::size_t remaining = 0; // bands taken but not yet finished, plus the untaken ones
    std::exception_ptr error;  // the first band that threw; rethrown on the submitter
    std::condition_variable done;
};

// Depth of band bodies this thread is inside. Non-zero means "a parallel call from here must run
// inline" -- the rule the whole deadlock argument rests on (see parallelBands' comment).
thread_local int t_bandDepth = 0;

// Run every band of a partition on the calling thread, in order. Used for a single-band job, for
// a nested call, and when no workers exist at all (a one-core machine, or after shutdown).
void runInline(detail::BandFn fn, void* ctx, std::size_t bands) {
    for (std::size_t b = 0; b < bands; ++b)
        fn(ctx, b);
}

class Pool {
public:
    void run(std::size_t bands, detail::BandFn fn, void* ctx);
    void shutdown() noexcept;
    [[nodiscard]] std::size_t workerCount();

private:
    void startLocked();
    void workerLoop();
    void drainLocked(std::unique_lock<std::mutex>& lk);
    void runBandLocked(Job& job, std::size_t band, std::unique_lock<std::mutex>& lk);

    std::mutex m_m;
    std::condition_variable m_work;
    std::deque<Job*> m_jobs; // oldest first: a job is dropped once every band of it is claimed
    std::vector<std::thread> m_workers;
    bool m_stop = false;
};

// Start the workers on first use. Failing to start one is not fatal: the pool runs with however
// many it got (possibly none), because the submitter can always finish a job by itself.
void Pool::startLocked() {
    if (!m_workers.empty() || m_stop)
        return;
    const std::size_t n = hardwareThreads() - 1; // the submitting thread is the other one
    m_workers.reserve(n);
    try {
        for (std::size_t i = 0; i < n; ++i)
            m_workers.emplace_back([this] { workerLoop(); });
    } catch (...) {
        // Out of threads (rlimit, memory): carry on with what started.
    }
}

void Pool::workerLoop() {
    std::unique_lock lk(m_m);
    for (;;) {
        m_work.wait(lk, [this] { return m_stop || !m_jobs.empty(); });
        drainLocked(lk); // drain even when stopping: a submitter may still be waiting on us
        if (m_stop)
            return;
    }
}

// Claim and run bands until the queue is empty. Called with the lock held and returns with it
// held; the lock is never held while a band body runs.
void Pool::drainLocked(std::unique_lock<std::mutex>& lk) {
    while (!m_jobs.empty()) {
        Job* job = m_jobs.front();
        if (job->claimed >= job->bands) {
            m_jobs.pop_front(); // fully claimed: nothing left here for a worker to take
            continue;
        }
        runBandLocked(*job, job->claimed++, lk);
    }
}

void Pool::runBandLocked(Job& job, std::size_t band, std::unique_lock<std::mutex>& lk) {
    const detail::BandFn fn = job.fn;
    void* const ctx = job.ctx;
    lk.unlock();
    std::exception_ptr err;
    ++t_bandDepth;
    try {
        fn(ctx, band);
    } catch (...) {
        err = std::current_exception();
    }
    --t_bandDepth;
    lk.lock();
    if (err && !job.error)
        job.error = err;
    // Notified WITH THE LOCK HELD, deliberately: `job` (and its condition variable) lives on the
    // submitter's stack, and the submitter cannot leave its wait -- so cannot destroy it -- until
    // it reacquires this mutex, which is strictly after notify_all() has returned here.
    if (--job.remaining == 0)
        job.done.notify_all();
}

void Pool::run(std::size_t bands, detail::BandFn fn, void* ctx) {
    if (bands == 0)
        return;
    if (bands == 1 || t_bandDepth > 0) {
        runInline(fn, ctx, bands);
        return;
    }
    Job job;
    job.fn = fn;
    job.ctx = ctx;
    job.bands = bands;
    job.remaining = bands;

    std::unique_lock lk(m_m);
    startLocked();
    if (m_workers.empty()) { // nobody to help: never publish a job that only we could run
        lk.unlock();
        runInline(fn, ctx, bands);
        return;
    }
    m_jobs.push_back(&job);
    // One wakeup per band we are asking someone else to take, never more (a two-band job must not
    // wake 63 workers). A notify that lands on nobody -- every worker busy rather than parked --
    // is not a lost wakeup: a busy worker re-reads the queue between bands.
    if (bands - 1 >= m_workers.size()) {
        m_work.notify_all();
    } else {
        for (std::size_t i = 0; i + 1 < bands; ++i)
            m_work.notify_one();
    }

    // Take every band nobody else has: this is what guarantees the job completes even if the
    // workers are all busy with someone else's job (or never started at all).
    while (job.claimed < job.bands)
        runBandLocked(job, job.claimed++, lk);

    // Our stack frame is about to outlive the queue entry, so drop it ourselves unless a worker
    // already did. Both happen under the lock, and past this point the job is unreachable from
    // the queue -- only threads that already claimed a band still touch it, and `remaining`
    // keeps us here until they are done.
    for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
        if (*it == &job) {
            m_jobs.erase(it);
            break;
        }
    }
    job.done.wait(lk, [&job] { return job.remaining == 0; });
    const std::exception_ptr err = job.error;
    lk.unlock();
    if (err)
        std::rethrow_exception(err);
}

void Pool::shutdown() noexcept {
    if (t_bandDepth > 0)
        return; // a worker joining itself; nothing sane to do but decline
    std::vector<std::thread> workers;
    {
        const std::lock_guard lk(m_m);
        if (m_workers.empty())
            return;
        m_stop = true;
        workers.swap(m_workers); // no new job can start workers while m_stop is set
    }
    m_work.notify_all();
    for (std::thread& t : workers)
        t.join();
    const std::lock_guard lk(m_m);
    m_stop = false; // the next parallel call starts a fresh set
}

std::size_t Pool::workerCount() {
    const std::lock_guard lk(m_m);
    return m_workers.size();
}

// The pool itself. Deliberately never destroyed -- destroying it would race any thread still
// finishing a band during exit -- but reachable from this pointer, so it is not a leak by
// LeakSanitizer's definition either. The atexit hook joins the workers, which is the part of
// shutdown that actually matters; it runs before LSan's end-of-run check, so the worker stacks
// are gone by the time that looks.
Pool& pool() {
    static Pool* const p = [] {
        auto* created = new Pool();
        std::atexit(+[] { shutdownThreadPool(); });
        return created;
    }();
    return *p;
}

} // namespace

unsigned hardwareThreads() noexcept {
    static const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    return n;
}

std::size_t poolWorkerCount() noexcept { return pool().workerCount(); }

bool insidePoolBand() noexcept { return t_bandDepth > 0; }

void shutdownThreadPool() noexcept { pool().shutdown(); }

namespace detail {

void runBands(std::size_t bands, BandFn fn, void* ctx) { pool().run(bands, fn, ctx); }

} // namespace detail

} // namespace mosaic::common
