#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/profiler.hpp"

using namespace mosaic;
using common::Lane;
using common::ProfileRow;
using common::Profiler;

namespace {

// The compiled-in DEFAULT of the collection switch, captured at static-initialisation time --
// before any test case can toggle it, and before doctest's runner exists. `Profiler::s_enabled` is
// a constant-initialised std::atomic<bool>, so this observes the build-type default rather than a
// race. Nothing else in the test binary parses --profile / MOSAIC_PROFILE, so this IS the default.
const bool kDefaultEnabled = Profiler::enabled();

// Save/restore the process-wide switch and wipe the shared singleton, so a test that measures
// "what got recorded" cannot see rows another test (or another module's MOSAIC_PERF_SCOPE, which
// debug builds run with collection ON) happened to leave behind.
class ProfilerGuard {
public:
    explicit ProfilerGuard(bool on) : m_saved(Profiler::enabled()) {
        Profiler::instance().clear();
        Profiler::setEnabled(on);
    }
    ~ProfilerGuard() {
        Profiler::setEnabled(m_saved);
        Profiler::instance().clear();
    }
    ProfilerGuard(const ProfilerGuard&) = delete;
    ProfilerGuard& operator=(const ProfilerGuard&) = delete;

private:
    bool m_saved;
};

[[nodiscard]] std::optional<ProfileRow> rowFor(const std::string& name) {
    const std::vector<ProfileRow> rows = Profiler::instance().snapshot();
    const auto it = std::find_if(rows.begin(), rows.end(),
                                 [&](const ProfileRow& r) { return r.name == name; });
    if (it == rows.end())
        return std::nullopt;
    return *it;
}

// Enough work that steady_clock cannot report a zero interval on any sane platform.
void burn(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

// ---------------------------------------------------------------------------------------------
// Gating. The contract (docs/s60-performance-plan.md §8.1, S60-alpha): COLLECTION ships in every
// build behind a runtime flag, because the user runs the release build and an optimisation arc
// that cannot measure its target is guesswork. Debug defaults ON so the Help-menu FPS readout and
// Timing Profiler window keep working with no flag; release defaults OFF and is opted into with
// --profile / MOSAIC_PROFILE=1. The WINDOW stays debug-only, which is a src/ui concern, not this
// collector's -- moving the collector into common (S60-a) must not have changed any of it.
// ---------------------------------------------------------------------------------------------

TEST_CASE("profiler: the collection default follows the build type") {
#ifdef MOSAIC_DEBUG
    CHECK(kDefaultEnabled); // debug: on with no flag, exactly as before S60-alpha
#else
    CHECK_FALSE(kDefaultEnabled); // release: off until --profile / MOSAIC_PROFILE=1
#endif
}

TEST_CASE("profiler: the switch is settable in every build, and readable back") {
    const bool saved = Profiler::enabled();
    Profiler::setEnabled(true);
    CHECK(Profiler::enabled()); // what --profile / MOSAIC_PROFILE=1 do, in ANY build
    Profiler::setEnabled(false);
    CHECK_FALSE(Profiler::enabled());
    Profiler::setEnabled(saved);
}

TEST_CASE("profiler: a scope records nothing while collection is off") {
    ProfilerGuard guard(false);
    {
        MOSAIC_PERF_SCOPE("test op (off)", Lane::Cpu);
        burn(1);
    }
    CHECK_FALSE(rowFor("test op (off)").has_value());
    CHECK(Profiler::instance().snapshot().empty());
}

TEST_CASE("profiler: a scope records under its own name and lane once collection is on") {
    ProfilerGuard guard(true);
    {
        MOSAIC_PERF_SCOPE("test op (gpu)", Lane::Gpu);
        burn(2);
    }
    const std::optional<ProfileRow> row = rowFor("test op (gpu)");
    REQUIRE(row.has_value());
    CHECK(row->lane == Lane::Gpu);
    CHECK(row->count == 1);
    CHECK(row->last > 0.0);
    CHECK(row->min == doctest::Approx(row->last));
    CHECK(row->max == doctest::Approx(row->last));
    CHECK(row->avg == doctest::Approx(row->last));
    // A row is keyed by (name, lane), not by name alone: the same operation timed on both lanes is
    // two rows, which is what makes "one feature spanning both lanes" legible in the table.
    CHECK_FALSE(rowFor("test op (cpu)").has_value());
}

TEST_CASE("profiler: (name, lane) is the key -- the same name on two lanes is two rows") {
    ProfilerGuard guard(true);
    Profiler::instance().record("two lanes", Lane::Cpu, 1.0);
    Profiler::instance().record("two lanes", Lane::Gpu, 2.0);
    const std::vector<ProfileRow> rows = Profiler::instance().snapshot();
    const auto n = std::count_if(rows.begin(), rows.end(),
                                 [](const ProfileRow& r) { return r.name == "two lanes"; });
    CHECK(n == 2);
}

TEST_CASE("profiler: GpuDevice is a THIRD key, so submit time and device time sit side by side") {
    // S60-a, docs/s60-performance-plan.md section 8.1. `Lane::Gpu` is wall-clock at the call site
    // (host work + submit + fence); `Lane::GpuDevice` is what a timestamp query says the device
    // actually spent. The same name on both is deliberate -- the GAP between them is the
    // diagnostic, and collapsing them into one row is what hid it for the whole arc so far.
    ProfilerGuard guard(true);
    Profiler::instance().record("Tile composite", Lane::Cpu, 1.0);
    Profiler::instance().record("Tile composite", Lane::Gpu, 8.0);
    Profiler::instance().record("Tile composite", Lane::GpuDevice, 2.0);
    const std::vector<ProfileRow> rows = Profiler::instance().snapshot();
    const auto n = std::count_if(rows.begin(), rows.end(),
                                 [](const ProfileRow& r) { return r.name == "Tile composite"; });
    CHECK(n == 3);
    const auto dev = std::find_if(rows.begin(), rows.end(), [](const ProfileRow& r) {
        return r.name == "Tile composite" && r.lane == Lane::GpuDevice;
    });
    REQUIRE(dev != rows.end());
    CHECK(dev->last == doctest::Approx(2.0));
}

TEST_CASE("profiler: every lane has exactly one three-character tag, defined in one place") {
    // The Timing Profiler window's pill and the --profile stderr dump both print this, and a
    // lane added without a tag used to fall through a ternary and silently render as "CPU".
    CHECK(std::string(common::laneName(Lane::Cpu)) == "CPU");
    CHECK(std::string(common::laneName(Lane::Gpu)) == "GPU");
    CHECK(std::string(common::laneName(Lane::GpuDevice)) == "DEV");
}

TEST_CASE("profiler: the enabled state is latched at scope CONSTRUCTION, not at destruction") {
    // Toggling collection mid-scope must not record a duration measured from a start that was
    // never taken (switching ON mid-scope) nor drop one that was (switching OFF mid-scope).
    ProfilerGuard guard(false);
    {
        MOSAIC_PERF_SCOPE("latched off", Lane::Cpu);
        Profiler::setEnabled(true); // arrived late: this scope stays un-timed
    }
    CHECK_FALSE(rowFor("latched off").has_value());

    {
        MOSAIC_PERF_SCOPE("latched on", Lane::Cpu);
        burn(1);
        Profiler::setEnabled(false); // left early: the scope still lands, it was already timing
    }
    CHECK(rowFor("latched on").has_value());
}

// ---------------------------------------------------------------------------------------------
// Nesting and re-entry. Scopes are added freely inside code that recurses (the compositor's tree
// walk) and inside code that already sits under an outer scope (uploadSampledTexture's host-copy
// and fence-wait rows live inside its own total), so both shapes have to behave.
// ---------------------------------------------------------------------------------------------

TEST_CASE("profiler: nested scopes each record, and the outer contains the inner") {
    ProfilerGuard guard(true);
    {
        MOSAIC_PERF_SCOPE("outer", Lane::Gpu);
        burn(1);
        {
            MOSAIC_PERF_SCOPE("inner", Lane::Cpu);
            burn(3);
        }
        burn(1);
    }
    const std::optional<ProfileRow> outer = rowFor("outer");
    const std::optional<ProfileRow> inner = rowFor("inner");
    REQUIRE(outer.has_value());
    REQUIRE(inner.has_value());
    CHECK(outer->lane == Lane::Gpu);
    CHECK(inner->lane == Lane::Cpu);
    CHECK(outer->count == 1);
    CHECK(inner->count == 1);
    CHECK(outer->last >= inner->last); // wall-clock containment, not a sum
}

TEST_CASE("profiler: re-entering the same name accumulates instead of replacing") {
    ProfilerGuard guard(true);
    // Two scopes of the SAME name live at once, as a recursive walk produces. Both land, and the
    // stats aggregate: count 2, min/max bracketing, avg between them.
    {
        MOSAIC_PERF_SCOPE("recursive", Lane::Cpu);
        burn(3);
        {
            MOSAIC_PERF_SCOPE("recursive", Lane::Cpu);
            burn(1);
        }
    }
    const std::optional<ProfileRow> row = rowFor("recursive");
    REQUIRE(row.has_value());
    CHECK(row->count == 2);
    CHECK(row->min <= row->max);
    CHECK(row->avg >= row->min);
    CHECK(row->avg <= row->max);
    // The INNER scope closes first, so `last` is the outer one -- the longer of the two.
    CHECK(row->last == doctest::Approx(row->max));
}

TEST_CASE("profiler: repeated records aggregate min/max/avg/count") {
    ProfilerGuard guard(true);
    Profiler::instance().record("agg", Lane::Cpu, 10.0);
    Profiler::instance().record("agg", Lane::Cpu, 2.0);
    Profiler::instance().record("agg", Lane::Cpu, 6.0);
    const std::optional<ProfileRow> row = rowFor("agg");
    REQUIRE(row.has_value());
    CHECK(row->count == 3);
    CHECK(row->min == doctest::Approx(2.0));
    CHECK(row->max == doctest::Approx(10.0));
    CHECK(row->avg == doctest::Approx(6.0));
    CHECK(row->last == doctest::Approx(6.0));
}

TEST_CASE("profiler: snapshot is sorted slowest-first, so the expensive rows stay on top") {
    ProfilerGuard guard(true);
    Profiler::instance().record("fast", Lane::Cpu, 1.0);
    Profiler::instance().record("slow", Lane::Cpu, 500.0);
    Profiler::instance().record("middling", Lane::Gpu, 50.0);
    const std::vector<ProfileRow> rows = Profiler::instance().snapshot();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].name == "slow");
    CHECK(rows[1].name == "middling");
    CHECK(rows[2].name == "fast");
}

TEST_CASE("profiler: heat highlights a recently-slow row and leaves fast ones alone") {
    ProfilerGuard guard(true);
    Profiler::instance().record("cheap", Lane::Cpu, Profiler::kHotMs - 1.0);
    Profiler::instance().record("dear", Lane::Cpu, Profiler::kVeryHotMs + 10.0);
    const std::optional<ProfileRow> cheap = rowFor("cheap");
    const std::optional<ProfileRow> dear = rowFor("dear");
    REQUIRE(cheap.has_value());
    REQUIRE(dear.has_value());
    CHECK(cheap->heat == doctest::Approx(0.0)); // below kHotMs: never highlighted
    CHECK(dear->heat > 0.9);                    // just fired and far past kVeryHotMs
    CHECK(dear->heat <= 1.0);
}

// ---------------------------------------------------------------------------------------------
// frameTick / fps. Deliberately NOT compiled out of release: the Timing Profiler window's own
// header line is frame rate and frame time, so a release --profile run without it would open a
// window that cannot say how fast the app is running.
// ---------------------------------------------------------------------------------------------

TEST_CASE("profiler: frameTick feeds the smoothed fps readout in every build") {
    ProfilerGuard guard(true);
    CHECK(Profiler::instance().fps() == doctest::Approx(0.0)); // cleared: no frames yet
    for (int i = 0; i < 200; ++i)
        Profiler::instance().frameTick(20.0);
    CHECK(Profiler::instance().frameMs() == doctest::Approx(20.0).epsilon(0.01));
    CHECK(Profiler::instance().fps() == doctest::Approx(50.0).epsilon(0.01));
    // Nonsense intervals are refused rather than smoothed in: a first frame with no prior stamp
    // reports 0, and a multi-second gap (the app was in the background) is not a frame time.
    Profiler::instance().frameTick(0.0);
    Profiler::instance().frameTick(-5.0);
    Profiler::instance().frameTick(5000.0);
    CHECK(Profiler::instance().frameMs() == doctest::Approx(20.0).epsilon(0.01));
}

TEST_CASE("profiler: clear drops both the rows and the frame average") {
    ProfilerGuard guard(true);
    Profiler::instance().record("gone", Lane::Cpu, 1.0);
    Profiler::instance().frameTick(16.0);
    Profiler::instance().clear();
    CHECK(Profiler::instance().snapshot().empty());
    CHECK(Profiler::instance().frameMs() == doctest::Approx(0.0));
    CHECK(Profiler::instance().fps() == doctest::Approx(0.0));
}

TEST_CASE("profiler: recording from several threads at once is safe") {
    // Some work runs off the UI thread (the inpaint worker, the compositor's pool), so record()
    // is mutex-guarded. This pins that the counts add up rather than tearing.
    ProfilerGuard guard(true);
    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
        workers.emplace_back([] {
            for (int i = 0; i < kPerThread; ++i)
                Profiler::instance().record("threaded", Lane::Cpu, 1.0);
        });
    for (std::thread& w : workers)
        w.join();
    const std::optional<ProfileRow> row = rowFor("threaded");
    REQUIRE(row.has_value());
    CHECK(row->count == static_cast<std::uint64_t>(kThreads) * kPerThread);
}
