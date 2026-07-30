#include "core/text/spell_worker.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#include "core/text/spell_checker.hpp"

namespace mosaic::core::text {

struct SpellCheckWorker::Impl {
    // A queued scan. Only `pending` (the newest) is ever run -- older ones are overwritten before the
    // worker picks them up (coalescing).
    struct Job {
        TextBlock block;
        std::string docDef;
        std::string appDef;
        SpellScanOptions opts;
        std::size_t paraFirst = 0;
        std::size_t paraLast = 0;
        std::uint64_t epoch = 0;
    };

    // The enchant checker and its guard. A scan locks it for the whole scan; a UI suggest/add/ignore
    // locks it briefly -- they share the one broker so there is no cross-thread enchant access.
    SpellChecker checker;
    std::mutex checkerMx;

    // Job/result state.
    std::mutex mx;
    std::condition_variable cv;
    Job pending;
    bool hasJob = false;
    bool stop = false;
    Result result;
    bool hasResult = false;

    std::thread thread;

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mx);
                cv.wait(lk, [this] { return hasJob || stop; });
                if (stop) return;
                job = std::move(pending);  // take the latest queued request
                hasJob = false;
            }
            std::vector<MisspelledRange> ranges;
            {
                std::lock_guard<std::mutex> ck(checkerMx);
                ranges = scanBlockSpelling(job.block, checker, job.docDef, job.appDef, job.opts,
                                           job.paraFirst, job.paraLast);
            }
            {
                std::lock_guard<std::mutex> lk(mx);
                result = Result{job.epoch, std::move(ranges)};
                hasResult = true;  // supersedes any earlier untaken result
            }
        }
    }
};

SpellCheckWorker::SpellCheckWorker() : m_impl(std::make_unique<Impl>()) {
    // The Impl address is stable (held by unique_ptr), so the thread's raw pointer stays valid.
    Impl* p = m_impl.get();
    m_impl->thread = std::thread([p] { p->run(); });
}

SpellCheckWorker::~SpellCheckWorker() {
    if (!m_impl) return;
    {
        std::lock_guard<std::mutex> lk(m_impl->mx);
        m_impl->stop = true;
    }
    m_impl->cv.notify_one();
    if (m_impl->thread.joinable()) m_impl->thread.join();
}

void SpellCheckWorker::request(TextBlock block, std::string documentDefault, std::string appDefault,
                               SpellScanOptions opts, std::uint64_t epoch, std::size_t paraFirst,
                               std::size_t paraLast) {
    {
        std::lock_guard<std::mutex> lk(m_impl->mx);
        m_impl->pending = Impl::Job{std::move(block),      std::move(documentDefault),
                                    std::move(appDefault), opts,
                                    paraFirst,             paraLast,
                                    epoch};
        m_impl->hasJob = true;
    }
    m_impl->cv.notify_one();
}

std::optional<SpellCheckWorker::Result> SpellCheckWorker::takeResult() {
    std::lock_guard<std::mutex> lk(m_impl->mx);
    if (!m_impl->hasResult) return std::nullopt;
    m_impl->hasResult = false;
    return std::move(m_impl->result);
}

std::vector<std::string> SpellCheckWorker::suggest(std::string_view word, std::string_view language) {
    std::lock_guard<std::mutex> ck(m_impl->checkerMx);
    return m_impl->checker.suggest(word, language);
}

void SpellCheckWorker::addToUserDict(std::string_view word, std::string_view language) {
    std::lock_guard<std::mutex> ck(m_impl->checkerMx);
    m_impl->checker.addToUserDict(word, language);
}

void SpellCheckWorker::ignore(std::string_view word) {
    std::lock_guard<std::mutex> ck(m_impl->checkerMx);
    m_impl->checker.ignore(word);
}

bool SpellCheckWorker::hasDictionary(std::string_view language) {
    std::lock_guard<std::mutex> ck(m_impl->checkerMx);
    return m_impl->checker.hasDictionary(language);
}

void SpellCheckWorker::loadMockDictionary(
    std::string_view language, std::vector<std::string> misspelled,
    std::vector<std::pair<std::string, std::vector<std::string>>> suggestions) {
    std::lock_guard<std::mutex> ck(m_impl->checkerMx);
    m_impl->checker.loadMockDictionary(language, std::move(misspelled), std::move(suggestions));
}

}  // namespace mosaic::core::text
