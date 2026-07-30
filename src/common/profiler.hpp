#pragma once

// A named-operation performance profiler: the canvas FPS readout (which needs a smoothed frame
// rate) and the Timing Profiler window (a per-operation min/max/avg table), both reached from the
// Help menu. Those two pieces of UI stay DEBUG-ONLY -- a deliberate product decision (2026-07-18):
// the user wants ballpark numbers while developing, not diagnostic surfaces shipped in the product.
// The WINDOW that renders this table lives in `src/ui/timing_graph_window.{hpp,cpp}`; only the
// collector is here.
//
// ⚠ COLLECTION, however, is available in RELEASE since S60-alpha, and that distinction is the whole
// point. The user runs the release build (the debug compositor is 5-10x slower), so a profiler that
// compiles out of release cannot measure the configuration anyone actually experiences -- and an
// optimisation arc that cannot measure its target is guesswork. See
// docs/s60-performance-plan.md section 8.
//
// ⚠ WHY IT LIVES IN `common` (S60-a, moved out of `src/ui/`): the two biggest uninstrumented costs
// in the app are in `src/render/` -- `WindowRenderer::uploadSampledTexture`'s two document-sized
// memcpys plus a blocking fence, and the adjustment step of `compositor.cpp`'s tree walk -- and
// `render` cannot include from `ui` without inverting the module layering (ui depends on render,
// never the reverse). A profiler is infrastructure, not UI, so it belongs in the FLTK-free base
// both `core` and `render` already depend on. Keep this header FLTK-free, Vulkan-free and
// platform-free, like the rest of `common`.
//
// Instrument a block with MOSAIC_PERF_SCOPE("<what>", Lane::Cpu|Gpu): it times the enclosing scope
// with a steady_clock and records the result under that (name, lane). Collection is OFF by default
// and switched on at startup by `--profile` / MOSAIC_PROFILE=1 (debug builds default it ON, so the
// Help-menu diagnostics keep working with no flag). When off, a site costs one relaxed atomic load
// and a predictable branch -- nothing next to what it measures, so sprinkle them freely.
//
// ⚠ Scope only REAL work. A scope on a walk that is usually a no-op (the `updateReflectionEnv`
// lesson: two dynamic_casts per layer per frame just to discover there were no text layers) buries
// the case that actually costs something under a row of near-zero samples.
//
// The Profiler is a process-wide, mutex-guarded singleton: some work runs off the UI thread (the
// inpaint worker), and the lock is only ever taken when collection is on.
//
// ---- THE THREE LANES (S60-a; docs/s60-performance-plan.md section 8.1) ------------------------
//
// Lane::Cpu       -- wall-clock of host work.
// Lane::Gpu       -- wall-clock AT THE CALL SITE of work that ends up on the device. It includes
//                    the host half (planning, descriptor writes, staging memcpys), the submit,
//                    and any fence wait. It is what the caller PAYS.
// Lane::GpuDevice -- real device execution time, read back from a Vulkan timestamp query pool
//                    (render::GpuTimer). It is what the device SPENT.
//
// Rows are keyed by (name, lane), so the same name on Lane::Gpu and Lane::GpuDevice is deliberate
// and is the pair worth reading: `Gpu` much greater than `GpuDevice` means the cost is host-side
// or fence-bound, and no amount of shader optimisation will touch it. Before S60-a there was no
// GpuDevice lane at all and every Gpu row was quietly reporting submit wall-clock as if it were
// device time -- which is what section 8.1 of the plan calls "measuring the wrong thing".

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The lane an operation runs on. GpuDevice is NOT "a faster GPU": it is the same work measured on
// the device timeline instead of the host one -- see the three-lane note above.
namespace mosaic::common {
enum class Lane { Cpu, Gpu, GpuDevice };

// The tag every consumer prints, defined once so the Timing Profiler window, the `--profile`
// stderr dump and anything added later cannot disagree about what a lane is called. Three
// characters on purpose: the window's lane column is a fixed-width pill.
[[nodiscard]] constexpr const char* laneName(Lane l) noexcept {
    switch (l) {
    case Lane::Gpu:
        return "GPU";
    case Lane::GpuDevice:
        return "DEV";
    case Lane::Cpu:
        break;
    }
    return "CPU";
}
}  // namespace mosaic::common

namespace mosaic::common {

// One row of the profiler snapshot -- the per-(name, lane) statistics the window renders. All times
// are milliseconds.
struct ProfileRow {
    std::string name;
    Lane lane = Lane::Cpu;
    double last = 0.0;
    double min = 0.0;
    double max = 0.0;
    double avg = 0.0;
    std::uint64_t count = 0;
    double ageSec = 0.0; // wall-clock seconds since this op last fired
    double heat = 0.0;   // 0..1 decaying "recently-slow" highlight (see snapshot())
};

class Profiler {
public:
    // Tuning for the decaying highlight: an op whose LAST sample was slow (>= kHotMs) glows, and the
    // glow fades over ~kHeatTauSec so a one-off spike stays catchable on screen for a few seconds.
    static constexpr double kHeatTauSec = 2.5;
    static constexpr double kHotMs = 4.0;      // below this an op is "fast" -- never highlighted
    static constexpr double kVeryHotMs = 40.0; // at/above this the highlight is fully saturated

    // Collection master switch. Read on every MOSAIC_PERF_SCOPE, so it is a plain relaxed load --
    // this is a diagnostic counter, not a synchronisation point, and a sample landing on either
    // side of a toggle is of no consequence.
    static bool enabled() noexcept { return s_enabled.load(std::memory_order_relaxed); }
    static void setEnabled(bool on) noexcept { s_enabled.store(on, std::memory_order_relaxed); }

    static Profiler& instance();

    // Record one timing of `name` on `lane` (milliseconds). Thread-safe.
    void record(std::string_view name, Lane lane, double ms);

    // Feed the wall-clock interval between two frames (ms) into the smoothed FPS the header shows.
    void frameTick(double intervalMs);

    [[nodiscard]] double fps() const;
    [[nodiscard]] double frameMs() const;

    // A copy of the per-op stats, SORTED SLOWEST-FIRST (by max), so the operations that take a long
    // time to render/composite stay at the top of the table. Each row also carries a decaying `heat`
    // so a recently-slow op glows and lingers for a few seconds even after it stops firing.
    [[nodiscard]] std::vector<ProfileRow> snapshot() const;

    void clear();

    // RAII: times its lifetime and records it on destruction. Constructed by MOSAIC_PERF_SCOPE.
    // The enabled state is latched at CONSTRUCTION, not re-read in the destructor: toggling
    // collection mid-scope would otherwise record a duration measured from an un-taken start.
    class Scope {
    public:
        Scope(std::string_view name, Lane lane) noexcept
            : m_name(name), m_lane(lane), m_on(Profiler::enabled()) {
            if (m_on)
                m_t0 = std::chrono::steady_clock::now();
        }
        ~Scope() {
            if (!m_on)
                return;
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - m_t0)
                                  .count();
            Profiler::instance().record(m_name, m_lane, ms);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        std::string_view m_name; // points at a string literal at the call site (static storage)
        Lane m_lane;
        bool m_on;
        std::chrono::steady_clock::time_point m_t0;
    };

private:
    struct Entry {
        Lane lane = Lane::Cpu;
        std::string name;
        std::uint64_t count = 0;
        double last = 0.0;
        double min = 0.0;
        double max = 0.0;
        double sum = 0.0;
        std::chrono::steady_clock::time_point lastSeen;
    };

    mutable std::mutex m_mutex;
    std::map<std::pair<std::string, Lane>, Entry> m_entries;
    double m_avgFrameMs = 0.0;

    // Debug builds default ON so the Help-menu FPS readout and Timing Profiler work with no flag,
    // exactly as they did before S60-alpha. Release defaults OFF and is opted into. The initialiser
    // lives in profiler.cpp -- ONE translation unit decides it, so a consumer compiled without
    // MOSAIC_DEBUG can never disagree with one compiled with it.
    static std::atomic<bool> s_enabled;
};

} // namespace mosaic::common

// TRANSITIONAL (S60-a): the collector was `mosaic::ui::Profiler` until this move, and the
// `src/ui/` call sites spell `Profiler` / `Lane` unqualified from inside `namespace mosaic::ui`.
// Re-export the three names there so the move is an include-line change at those sites rather than
// a rewrite of every MOSAIC_PERF_SCOPE line in the most contended file in the tree. No dependency
// on `ui` is created -- these are names, not declarations of ui types. Drop this block (and qualify
// the ~20 call sites in app_window.cpp / vulkan_canvas.cpp) once those files are free.
namespace mosaic::ui {
using common::Lane;
using common::ProfileRow;
using common::Profiler;
} // namespace mosaic::ui

// Time the enclosing scope and record it under (name, lane). Unique variable name per line.
// Present in EVERY build; the runtime flag decides whether it costs anything.
#define MOSAIC_PERF_CONCAT_INNER(a, b) a##b
#define MOSAIC_PERF_CONCAT(a, b) MOSAIC_PERF_CONCAT_INNER(a, b)
#define MOSAIC_PERF_SCOPE(name, lane)                                                              \
    ::mosaic::common::Profiler::Scope MOSAIC_PERF_CONCAT(mosaicPerfScope_, __LINE__)               \
    {                                                                                              \
        (name), (lane)                                                                             \
    }
