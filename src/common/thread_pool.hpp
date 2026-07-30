#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <type_traits>

// The process-wide worker pool behind every band-parallel loop in Mosaic: the compositor walk,
// the blur kernels, the texture generators and the inpaint backends. Each of those grew its own
// copy of the same helper, and every copy built and JOINED A FRESH std::vector<std::thread> ON
// EVERY CALL. Thread creation costs ~20-60us on Linux and an interactive gesture drives dozens of
// those calls per frame (18 sites in the compositor alone, times the layers in the document), so
// the spawn was a measurable slice of the frame that bought nothing. The pool parks
// hardwareThreads() - 1 workers on a condition variable once and hands them BANDS instead.
//
// What the pool deliberately does NOT do is decide how the work is split. Every caller keeps its
// own band arithmetic verbatim (PLAN.md section 8.3 determinism contract: bands write disjoint
// ranges, so ANY band count reproduces the serial result bit for bit -- which is what keeps the
// golden images valid). parallelBands() only runs [0, bands) of a partition the caller already
// computed; parallelFor() is the one shared shape, copied unchanged from the helpers it replaced.
namespace mosaic::common {

// std::thread::hardware_concurrency(), clamped to >= 1 and cached. libstdc++ answers that call
// with a /sys read every time, which is pure syscall on a helper that runs dozens of times a
// frame; the value is a machine property, so caching it also pins the band count for the session.
[[nodiscard]] unsigned hardwareThreads() noexcept;

// Workers currently parked in the pool: hardwareThreads() - 1, because the submitting thread runs
// bands itself, which puts exactly hardwareThreads() threads on the work. 0 before the first
// parallel call starts the pool (it starts lazily) and 0 again after shutdownThreadPool().
[[nodiscard]] std::size_t poolWorkerCount() noexcept;

// True while the calling thread is inside a band body -- whether it is a pool worker or the
// thread that submitted the job and is running a band of its own. A parallel call made from
// inside a band runs INLINE on the calling thread (see parallelBands).
[[nodiscard]] bool insidePoolBand() noexcept;

// Stop and join the workers; a later parallel call starts a fresh set. Registered as a static
// destructor on first use so a normal process exit leaves no live threads behind, and exposed
// for tests. Never call it from inside a band body (a worker would join itself); doing so is a
// no-op rather than a crash.
void shutdownThreadPool() noexcept;

namespace detail {

// Type-erased band body: a trampoline plus the address of the caller's lambda. A raw pair rather
// than a std::function so a parallel call allocates nothing at all.
using BandFn = void (*)(void* ctx, std::size_t band);

void runBands(std::size_t bands, BandFn fn, void* ctx);

} // namespace detail

// Run body(b) for every b in [0, bands) and return only once all of them have finished. Bands may
// run on any thread and in any order, so a body must touch only state its own band owns.
//
// NESTING: if the calling thread is already inside a band, the whole range runs inline on this
// thread, in band order. That is the same answer (bands are disjoint by contract, so serial
// execution of every band IS the serial loop) and it is what makes deadlock structurally
// impossible: a band body never waits on the pool, so the only thread that ever blocks is a
// submitter -- and a submitter claims and runs every band nobody else took, so it can always
// finish its own job unaided, even if every worker is busy elsewhere or absent entirely.
//
// An exception thrown by a band is caught, the remaining bands still run to completion (the job
// lives on the submitter's stack; unwinding past it while a worker still reads it would be a
// dangling reference), and the first one is rethrown on the submitting thread.
template <typename Body>
void parallelBands(std::size_t bands, Body&& body) {
    using Fn = std::remove_reference_t<Body>;
    detail::runBands(
        bands, [](void* ctx, std::size_t b) { (*static_cast<Fn*>(ctx))(b); },
        const_cast<void*>(static_cast<const void*>(std::addressof(body))));
}

// The band split every pixel loop in the tree uses: [0, count) cut into contiguous equal-length
// bands, at most one per hardware thread and never more than 32, with fewer than `minPerBand`
// items per band left serial (spawning for a handful of pixels always lost). fn(i0, i1) gets each
// half-open range.
//
// The arithmetic is a VERBATIM copy of the per-file helpers this replaced -- same band count and
// the same [i0, i1) for every input -- because the golden images were rendered through it and the
// determinism contract is stated in terms of it. Do not "improve" the partitioning here.
template <typename Fn>
void parallelFor(std::size_t count, std::size_t minPerBand, Fn&& fn) {
    const std::size_t hw = hardwareThreads();
    const std::size_t bands =
        std::min({hw, count / std::max<std::size_t>(1, minPerBand), std::size_t{32}});
    if (bands <= 1) {
        fn(std::size_t{0}, count);
        return;
    }
    const std::size_t step = (count + bands - 1) / bands;
    // A rounded-up step can leave trailing bands empty; the spawning helpers broke out of their
    // loop when they hit one, so the job carries only the non-empty bands -- ceil(count / step)
    // of them, covering exactly the same ranges.
    parallelBands((count + step - 1) / step, [&](std::size_t b) {
        const std::size_t i0 = b * step;
        fn(i0, std::min(count, i0 + step));
    });
}

} // namespace mosaic::common
