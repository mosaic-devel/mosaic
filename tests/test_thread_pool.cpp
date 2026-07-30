#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "common/thread_pool.hpp"

using namespace mosaic;

namespace {

using Range = std::pair<std::size_t, std::size_t>;

// The helper as it stood BEFORE S60-b: a fresh std::vector<std::thread> per call. It is the
// reference the golden images were rendered through, so every partition the pool produces is
// compared against this -- band for band, byte for byte in the ranges it hands out.
template <typename Fn>
void spawnParallelFor(std::size_t count, std::size_t minPerBand, Fn&& fn) {
    const std::size_t hw = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t bands =
        std::min({hw, count / std::max<std::size_t>(1, minPerBand), std::size_t{32}});
    if (bands <= 1) {
        fn(std::size_t{0}, count);
        return;
    }
    std::vector<std::thread> workers;
    workers.reserve(bands);
    const std::size_t step = (count + bands - 1) / bands;
    for (std::size_t b = 0; b < bands; ++b) {
        const std::size_t i0 = b * step;
        const std::size_t i1 = std::min(count, i0 + step);
        if (i0 >= i1) break;
        workers.emplace_back([&fn, i0, i1] { fn(i0, i1); });
    }
    for (std::thread& t : workers) t.join();
}

// Every [i0, i1) a partition hands out, in ascending order (bands run in any order, so the
// collection is sorted before comparing).
std::vector<Range> poolRanges(std::size_t count, std::size_t minPerBand) {
    std::vector<Range> out;
    std::mutex m;
    common::parallelFor(count, minPerBand, [&](std::size_t i0, std::size_t i1) {
        const std::lock_guard lk(m);
        out.emplace_back(i0, i1);
    });
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Range> spawnRanges(std::size_t count, std::size_t minPerBand) {
    std::vector<Range> out;
    std::mutex m;
    spawnParallelFor(count, minPerBand, [&](std::size_t i0, std::size_t i1) {
        const std::lock_guard lk(m);
        out.emplace_back(i0, i1);
    });
    std::sort(out.begin(), out.end());
    return out;
}

// The sizes below are the ones the tree actually asks for: pixel counts and row counts at the
// minPerBand values used by the compositor (32/64/1<<15..1<<18), the blur kernels (2/8/4096) and
// the texture generators (16), plus the boundary cases around each.
const std::vector<std::pair<std::size_t, std::size_t>> kCases = {
    {0, 64},      {1, 64},      {63, 64},      {64, 64},      {65, 64},
    {127, 64},    {128, 64},    {129, 64},     {1080, 64},    {1080, 32},
    {720, 2},     {4095, 8},    {4096, 4096},  {8192, 4096},  {100000, 4096},
    {1920 * 1080, 1 << 16},     {1920 * 1080, 1 << 18},       {2073600, 1 << 15},
    {17, 1},      {1000, 0},    {33, 1},       {1023, 16},    {1024, 16},
};

} // namespace

// ---------------------------------------------------------------------------------------------
// The load-bearing property: the pool must partition exactly as the spawn-per-call helper did.
// ---------------------------------------------------------------------------------------------
TEST_CASE("parallelFor reproduces the spawn-per-call partition exactly") {
    for (const auto& [count, minPerBand] : kCases) {
        const std::vector<Range> pooled = poolRanges(count, minPerBand);
        const std::vector<Range> spawned = spawnRanges(count, minPerBand);
        CAPTURE(count);
        CAPTURE(minPerBand);
        REQUIRE(pooled.size() == spawned.size());
        for (std::size_t i = 0; i < pooled.size(); ++i) {
            CHECK(pooled[i].first == spawned[i].first);
            CHECK(pooled[i].second == spawned[i].second);
        }
    }
}

TEST_CASE("parallelFor bands are contiguous, disjoint and cover the whole range") {
    for (const auto& [count, minPerBand] : kCases) {
        const std::vector<Range> r = poolRanges(count, minPerBand);
        CAPTURE(count);
        CAPTURE(minPerBand);
        REQUIRE(!r.empty());
        CHECK(r.front().first == 0);
        CHECK(r.back().second == count);
        for (std::size_t i = 0; i + 1 < r.size(); ++i) {
            CHECK(r[i].second == r[i + 1].first); // no gap, no overlap
            CHECK(r[i].first < r[i].second);      // and never an empty band
        }
    }
}

TEST_CASE("the partition is a pure function of (count, minPerBand)") {
    for (const auto& [count, minPerBand] : kCases) {
        const std::vector<Range> first = poolRanges(count, minPerBand);
        for (int rep = 0; rep < 8; ++rep) {
            CHECK(poolRanges(count, minPerBand) == first);
        }
    }
}

TEST_CASE("an empty count still calls the body once, with an empty range") {
    int calls = 0;
    common::parallelFor(0, 64, [&](std::size_t i0, std::size_t i1) {
        ++calls;
        CHECK(i0 == 0);
        CHECK(i1 == 0);
    });
    CHECK(calls == 1);
}

TEST_CASE("parallelBands runs every band index exactly once") {
    for (const std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{64}}) {
        std::vector<std::atomic<int>> seen(n);
        for (auto& s : seen) s.store(0);
        common::parallelBands(n, [&](std::size_t b) {
            REQUIRE(b < n);
            seen[b].fetch_add(1);
        });
        for (std::size_t b = 0; b < n; ++b) CHECK(seen[b].load() == 1);
    }
    // Zero bands is a no-op, not a hang.
    int ranZero = 0;
    common::parallelBands(0, [&](std::size_t) { ++ranZero; });
    CHECK(ranZero == 0);
}

// ---------------------------------------------------------------------------------------------
// Correctness of the work itself.
// ---------------------------------------------------------------------------------------------
TEST_CASE("bands compute the same result as the serial loop") {
    std::vector<double> v(100000);
    std::iota(v.begin(), v.end(), 1.0);
    std::vector<double> pooled(v.size(), 0.0);
    common::parallelFor(v.size(), 64, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) pooled[i] = v[i] * 0.5 + 1.0;
    });
    for (std::size_t i = 0; i < v.size(); ++i) {
        REQUIRE(pooled[i] == v[i] * 0.5 + 1.0); // bit-exact, not approximate
    }
}

TEST_CASE("the submitting thread does not return before every band has finished") {
    // Every band's writes must be visible to the submitter the moment the call returns -- the
    // property every call site's "write into my slice, read it after" contract depends on.
    for (int rep = 0; rep < 200; ++rep) {
        std::vector<int> out(4096, 0);
        common::parallelFor(out.size(), 16, [&](std::size_t i0, std::size_t i1) {
            for (std::size_t i = i0; i < i1; ++i) out[i] = 1;
        });
        REQUIRE(std::count(out.begin(), out.end(), 1) == static_cast<long>(out.size()));
    }
}

// ---------------------------------------------------------------------------------------------
// Nesting: a parallel call from inside a band. A naive pool deadlocks here.
// ---------------------------------------------------------------------------------------------
TEST_CASE("a parallel call from inside a band runs inline on that thread") {
    CHECK(!common::insidePoolBand());
    std::atomic<int> innerBands{0};
    std::atomic<int> offThread{0};
    std::vector<int> out(65536, 0);
    common::parallelFor(out.size(), 16, [&](std::size_t i0, std::size_t i1) {
        CHECK(common::insidePoolBand());
        const std::thread::id self = std::this_thread::get_id();
        common::parallelFor(i1 - i0, 1, [&](std::size_t j0, std::size_t j1) {
            innerBands.fetch_add(1);
            if (std::this_thread::get_id() != self) offThread.fetch_add(1);
            for (std::size_t j = j0; j < j1; ++j) out[i0 + j] = 1;
        });
    });
    CHECK(!common::insidePoolBand());
    CHECK(offThread.load() == 0);          // nested bands never leave the calling thread
    CHECK(innerBands.load() > 0);
    CHECK(std::count(out.begin(), out.end(), 1) == static_cast<long>(out.size()));
}

TEST_CASE("nesting three deep completes and covers the range") {
    std::vector<int> out(1 << 16, 0);
    common::parallelFor(out.size(), 64, [&](std::size_t a0, std::size_t a1) {
        common::parallelFor(a1 - a0, 8, [&](std::size_t b0, std::size_t b1) {
            common::parallelFor(b1 - b0, 1, [&](std::size_t c0, std::size_t c1) {
                for (std::size_t c = c0; c < c1; ++c) out[a0 + b0 + c] = 1;
            });
        });
    });
    CHECK(std::count(out.begin(), out.end(), 1) == static_cast<long>(out.size()));
}

// ---------------------------------------------------------------------------------------------
// Contention: the compositor runs on the UI thread while the inpaint / texture / save workers
// run their own parallel loops off-thread.
// ---------------------------------------------------------------------------------------------
TEST_CASE("concurrent submitters each get a complete, correct result") {
    constexpr int kThreads = 8;
    constexpr int kRounds = 40;
    std::atomic<int> bad{0};
    std::vector<std::thread> callers;
    callers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        callers.emplace_back([&, t] {
            std::vector<int> out(20000, 0);
            for (int r = 0; r < kRounds; ++r) {
                std::fill(out.begin(), out.end(), 0);
                common::parallelFor(out.size(), 16, [&](std::size_t i0, std::size_t i1) {
                    for (std::size_t i = i0; i < i1; ++i) out[i] = t + r;
                });
                for (const int v : out) {
                    if (v != t + r) bad.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& c : callers) c.join();
    CHECK(bad.load() == 0);
}

TEST_CASE("concurrent submitters that also nest") {
    constexpr int kThreads = 6;
    std::atomic<int> bad{0};
    std::vector<std::thread> callers;
    callers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        callers.emplace_back([&] {
            std::vector<int> out(8192, 0);
            for (int r = 0; r < 30; ++r) {
                std::fill(out.begin(), out.end(), 0);
                common::parallelFor(out.size(), 32, [&](std::size_t i0, std::size_t i1) {
                    common::parallelFor(i1 - i0, 4, [&](std::size_t j0, std::size_t j1) {
                        for (std::size_t j = j0; j < j1; ++j) out[i0 + j] = 1;
                    });
                });
                if (std::count(out.begin(), out.end(), 1) != static_cast<long>(out.size())) {
                    bad.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& c : callers) c.join();
    CHECK(bad.load() == 0);
}

TEST_CASE("many tiny jobs in a row all complete (no lost wakeups)") {
    std::vector<int> out(2048, 0);
    for (int rep = 0; rep < 3000; ++rep) {
        std::fill(out.begin(), out.end(), 0);
        common::parallelFor(out.size(), 1, [&](std::size_t i0, std::size_t i1) {
            for (std::size_t i = i0; i < i1; ++i) out[i] = rep;
        });
        REQUIRE(out.front() == rep);
        REQUIRE(out.back() == rep);
    }
}

// ---------------------------------------------------------------------------------------------
// Failure paths.
// ---------------------------------------------------------------------------------------------
TEST_CASE("an exception from a band reaches the submitter and leaves the pool usable") {
    std::atomic<int> ran{0};
    CHECK_THROWS_AS(common::parallelFor(4096, 16,
                                        [&](std::size_t, std::size_t) {
                                            ran.fetch_add(1);
                                            throw std::runtime_error("band");
                                        }),
                    std::runtime_error);
    CHECK(ran.load() > 0);

    std::vector<int> out(4096, 0);
    common::parallelFor(out.size(), 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) out[i] = 7;
    });
    CHECK(std::count(out.begin(), out.end(), 7) == static_cast<long>(out.size()));
}

TEST_CASE("shutdown joins the workers and a later call restarts the pool") {
    std::vector<int> warm(4096, 0);
    common::parallelFor(warm.size(), 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) warm[i] = 1;
    });
    const bool multicore = common::hardwareThreads() > 1;
    if (multicore) CHECK(common::poolWorkerCount() == common::hardwareThreads() - 1);

    common::shutdownThreadPool();
    CHECK(common::poolWorkerCount() == 0);

    // With no workers the work still runs -- inline, on the submitting thread, same answer.
    std::vector<int> out(4096, 0);
    common::parallelFor(out.size(), 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) out[i] = 3;
    });
    CHECK(std::count(out.begin(), out.end(), 3) == static_cast<long>(out.size()));
    if (multicore) CHECK(common::poolWorkerCount() == common::hardwareThreads() - 1);

    // Twice in a row is a no-op, not a hang or a double join.
    common::shutdownThreadPool();
    common::shutdownThreadPool();
    CHECK(common::poolWorkerCount() == 0);
    common::parallelFor(out.size(), 16, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) out[i] = 5;
    });
    CHECK(std::count(out.begin(), out.end(), 5) == static_cast<long>(out.size()));
}

TEST_CASE("hardwareThreads is at least one and never changes") {
    const unsigned n = common::hardwareThreads();
    CHECK(n >= 1);
    for (int i = 0; i < 4; ++i) CHECK(common::hardwareThreads() == n);
}
