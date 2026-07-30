// GPU TIMESTAMP QUERIES (S60-a; docs/s60-performance-plan.md section 8.1).
//
// Two halves, tested two ways, because they fail differently:
//
//   * THE ARITHMETIC is pure and is where the subtle bugs live -- an unmasked subtraction on a
//     36-bit counter, or a counter turnover read as ~584 years. Those cases cannot be provoked on
//     the dev rig at all (its counter turns over every ~69 s and a test would have to wait for it),
//     so they are unit-tested against synthetic ticks. This is the same reason `GpuCaps::decide()`
//     is a pure function with a synthetic probe.
//
//   * THE PLUMBING needs a device, so it is proven end to end through `render::TileCompositor`,
//     which holds the timer: composite once with collection on and require that the profiler now
//     carries BOTH a `Lane::Gpu` row (submit wall-clock) and a `Lane::GpuDevice` row (real device
//     time) under the same name. That pairing IS the deliverable -- before this, every `Lane::Gpu`
//     row in the tree was submit wall-clock wearing a GPU label.
//
// CI-safe in the house pattern (test_extrude_gpu.cpp / test_blur_gpu.cpp): with no usable Vulkan
// device, or on a device with no timestamp counter, the device cases WARN and pass. Under
// `MOSAIC_GPU_PROFILE=floor` the caps gate refuses the timer by construction, and the consistency
// case below asserts exactly that rather than skipping it.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/geometry.hpp"
#include "common/image.hpp"
#include "common/profiler.hpp"
#include "core/document.hpp"
#include "core/layer.hpp"
#include "render/compositor.hpp"
#include "render/gpu_caps.hpp"
#include "render/gpu_timer.hpp"
#include "render/tile_compositor.hpp"

using mosaic::common::Lane;
using mosaic::common::ProfileRow;
using mosaic::common::Profiler;
using mosaic::render::maskTimestamp;
using mosaic::render::timestampDeltaMs;
namespace common = mosaic::common;
namespace core = mosaic::core;
namespace render = mosaic::render;

namespace {

// Save/restore the process-wide collection switch and wipe the singleton, so a case that asks
// "what got recorded" cannot see another module's rows. Same shape as test_profiler.cpp's guard.
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

[[nodiscard]] std::optional<ProfileRow> rowFor(const char* name, Lane lane) {
    const std::vector<ProfileRow> rows = Profiler::instance().snapshot();
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const ProfileRow& r) {
        return r.name == name && r.lane == lane;
    });
    if (it == rows.end())
        return std::nullopt;
    return *it;
}

std::unique_ptr<render::TileCompositor> makeLane(const char* who) {
    std::string err;
    auto lane = render::TileCompositor::create(err);
    if (!lane) {
        const std::string note =
            std::string("no usable Vulkan device -- skipping ") + who + " (" + err + ")";
        WARN_MESSAGE(true, note);
    }
    return lane;
}

// A two-layer document the tile lane definitely accepts: top-level rasters, no groups, no
// adjustments, no effects. Small, because this file is about instrumentation, not pixels.
void buildDoc(core::Document& doc) {
    for (int i = 0; i < 2; ++i) {
        auto l = doc.makeRaster("l", doc.width(), doc.height());
        common::Image& img = l->image();
        for (std::uint32_t y = 0; y < img.height; ++y)
            for (std::uint32_t x = 0; x < img.width; ++x) {
                const std::size_t p = (static_cast<std::size_t>(y) * img.width + x) * 4;
                img.rgba[p + 0] = static_cast<std::uint8_t>(x * 3 + i * 40);
                img.rgba[p + 1] = static_cast<std::uint8_t>(y * 5 + i * 20);
                img.rgba[p + 2] = static_cast<std::uint8_t>(i == 0 ? 200 : 60);
                img.rgba[p + 3] = static_cast<std::uint8_t>(i == 0 ? 255 : 170);
            }
        doc.root().addOnTop(std::move(l));
    }
}

render::CompositeOptions displayOptions() {
    render::CompositeOptions opts;
    opts.checkerboard = false;  // the lane refuses a baked checkerboard by design
    return opts;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The arithmetic
// ---------------------------------------------------------------------------------------------

TEST_CASE("maskTimestamp keeps only the bits the device promises are meaningful") {
    // Vulkan says the high 64 - timestampValidBits bits are UNDEFINED, not zero. A driver that
    // leaves garbage up there is conformant, and it is the reason this function exists.
    constexpr std::uint64_t kGarbageHigh = 0xFFFF'FF00'0000'0000ull;
    CHECK(maskTimestamp(kGarbageHigh | 0x1234ull, 36) == 0x1234ull);
    CHECK(maskTimestamp(0xFFFF'FFFF'FFFF'FFFFull, 36) == (std::uint64_t{1} << 36) - 1ull);

    // 0 and >= 64 both mean "nothing to mask" -- 64 because the whole counter is valid, 0 because
    // there is no counter and the caller has already been refused by the caps gate.
    CHECK(maskTimestamp(0xDEAD'BEEF'CAFE'F00Dull, 64) == 0xDEAD'BEEF'CAFE'F00Dull);
    CHECK(maskTimestamp(0xDEAD'BEEF'CAFE'F00Dull, 0) == 0xDEAD'BEEF'CAFE'F00Dull);
    CHECK(maskTimestamp(0xDEAD'BEEF'CAFE'F00Dull, 200) == 0xDEAD'BEEF'CAFE'F00Dull);
}

TEST_CASE("timestampDeltaMs converts ticks through the device's period") {
    // 1 ns/tick (the common desktop value): a million ticks is exactly one millisecond.
    CHECK(timestampDeltaMs(0, 1'000'000, 64, 1.0) == doctest::Approx(1.0));
    // A coarser counter: 40 ns/tick, 25'000 ticks -> 1 ms. The period is NOT assumed to be 1.
    CHECK(timestampDeltaMs(1000, 26'000, 64, 40.0) == doctest::Approx(1.0));
    // Sub-millisecond work still resolves; this is the range every macrotile dispatch lives in.
    CHECK(timestampDeltaMs(0, 1'500, 64, 1.0) == doctest::Approx(0.0015));
}

TEST_CASE("timestampDeltaMs reads a counter turnover as a short interval, not 584 years") {
    // The case a raw subtraction gets catastrophically wrong. On a 36-bit counter at 1 ns/tick the
    // turnover comes round every ~69 s, so a frame WILL straddle it during any long session.
    constexpr std::uint32_t kBits = 36;
    const std::uint64_t top = (std::uint64_t{1} << kBits) - 1000ull;  // 1000 ticks before the turn
    const std::uint64_t wrapped = 500;                                // 1500 ticks later
    CHECK(timestampDeltaMs(top, wrapped, kBits, 1.0) == doctest::Approx(0.0015));

    // And the undefined high bits must not leak into the answer even across a wrap.
    constexpr std::uint64_t kGarbageHigh = 0xABCD'0000'0000'0000ull;
    CHECK(timestampDeltaMs(kGarbageHigh | top, kGarbageHigh | wrapped, kBits, 1.0) ==
          doctest::Approx(0.0015));
}

TEST_CASE("timestampDeltaMs refuses to invent a number without a period") {
    // A device reporting no period cannot be converted. Zero is visible in a table; a garbage
    // number is not, and this row would sit at the top of a max-sorted profiler forever.
    CHECK(timestampDeltaMs(0, 1'000'000, 64, 0.0) == doctest::Approx(0.0));
    CHECK(timestampDeltaMs(0, 1'000'000, 64, -1.0) == doctest::Approx(0.0));
}

TEST_CASE("timestampDeltaMs is exact at zero and monotone in the interval") {
    CHECK(timestampDeltaMs(12345, 12345, 36, 1.0) == doctest::Approx(0.0));
    double prev = -1.0;
    for (std::uint64_t ticks : {1ull, 10ull, 1'000ull, 100'000ull, 10'000'000ull}) {
        const double ms = timestampDeltaMs(7, 7 + ticks, 36, 1.0);
        CHECK(ms > prev);
        prev = ms;
    }
}

// ---------------------------------------------------------------------------------------------
// The caps gate
// ---------------------------------------------------------------------------------------------

TEST_CASE("a Vulkan 1.0 floor device gets no timer, and says so in caps rather than crashing") {
    render::GpuProbe p;
    render::applyFloorProfile(p);
    const render::GpuCaps caps = render::decide(p);
    CHECK_FALSE(caps.timestampQueries);
    CHECK(caps.timestampValidBits == 0);
}

TEST_CASE("timestampValidBits is carried through decide() and gates the flag with the limits") {
    render::GpuProbe p;
    render::applyFloorProfile(p);
    p.limits.timestampComputeAndGraphics = VK_TRUE;
    p.limits.timestampPeriod = 1.0f;

    // Limits say yes, the queue family says it cannot write a timestamp: no timer. This is a real
    // configuration -- a transfer-only queue reports 0 valid bits -- and it is why the width is
    // probed from VkQueueFamilyProperties rather than assumed from the limit.
    p.timestampValidBits = 0;
    CHECK_FALSE(render::decide(p).timestampQueries);

    p.timestampValidBits = 36;  // Vulkan 1.0's guaranteed minimum where the limit is set
    const render::GpuCaps caps = render::decide(p);
    CHECK(caps.timestampQueries);
    CHECK(caps.timestampValidBits == 36);

    // And the width is cleared whenever the flag is off, so no caller can pick up a stray mask.
    p.limits.timestampPeriod = 0.0f;
    const render::GpuCaps noPeriod = render::decide(p);
    CHECK_FALSE(noPeriod.timestampQueries);
    CHECK(noPeriod.timestampValidBits == 0);
}

// ---------------------------------------------------------------------------------------------
// The plumbing, through the lane that holds the timer
// ---------------------------------------------------------------------------------------------

TEST_CASE("the tile lane's timer exists exactly when the device's caps say it may") {
    // Not a skip: this is the invariant that must hold under BOTH profiles. On the dev rig it
    // asserts the timer was built; under MOSAIC_GPU_PROFILE=floor it asserts it was refused.
    auto lane = makeLane("the tile lane's timer gate");
    if (!lane)
        return;
    CHECK(lane->hasDeviceTimer() == lane->caps().timestampQueries);
}

TEST_CASE("a composite records BOTH a submit row and a device row under the same name") {
    auto lane = makeLane("device-time profiler rows");
    if (!lane)
        return;
    if (!lane->hasDeviceTimer()) {
        WARN_MESSAGE(true, "device reports no timestamp counter -- skipping the device-row case");
        return;
    }

    core::Document doc(192, 128);
    buildDoc(doc);

    // Warm-up OUTSIDE the guard: the first composite builds the atlas and uploads both layers, so
    // its rows describe a cold lane. What is asserted below is a steady frame.
    lane->markAllDirty();
    REQUIRE(lane->composite(doc, displayOptions()).ok);

    const ProfilerGuard on(true);
    lane->markAllDirty();
    const render::TileCompositeStatus st = lane->composite(doc, displayOptions());
    REQUIRE(st.ok);

    // The pair that is the whole point: the same operation measured at the call site and on the
    // device. Before S60-a only the first existed, and it was labelled as if it were the second.
    const auto submit = rowFor("Tile composite", Lane::Gpu);
    const auto device = rowFor("Tile composite", Lane::GpuDevice);
    REQUIRE(submit.has_value());
    REQUIRE(device.has_value());
    CHECK(submit->count == 1);
    CHECK(device->count == 1);

    // The kernel's own row, which is the only one a shader change may move.
    const auto blend = rowFor("Tile blend", Lane::GpuDevice);
    REQUIRE(blend.has_value());

    // Sanity, not a benchmark: a device duration is non-negative and not absurd. An unmasked
    // subtraction on a 36-bit counter fails this by roughly eleven orders of magnitude, which is
    // precisely the bug the masking exists to prevent.
    CHECK(device->last >= 0.0);
    CHECK(device->last < 60'000.0);
    CHECK(blend->last >= 0.0);

    // The outer scope brackets the inner one in submission order (TOP_OF_PIPE before, BOTTOM_OF_PIPE
    // after), so the device timeline must agree. The tolerance is one tick's worth of slop, not a
    // hedge against the invariant being wrong.
    CHECK(blend->last <= device->last + 0.001);
}

TEST_CASE("with collection off, the timer writes no rows and the composite is unaffected") {
    auto lane = makeLane("the collection-off path");
    if (!lane)
        return;

    core::Document doc(160, 96);
    buildDoc(doc);
    lane->markAllDirty();
    REQUIRE(lane->composite(doc, displayOptions()).ok);
    common::Image withProfiler;
    std::string err;
    REQUIRE(lane->readback(common::Rect{}, withProfiler, err));

    {
        const ProfilerGuard off(false);
        lane->markAllDirty();
        REQUIRE(lane->composite(doc, displayOptions()).ok);
        CHECK_FALSE(rowFor("Tile composite", Lane::GpuDevice).has_value());
        CHECK_FALSE(rowFor("Tile composite", Lane::Gpu).has_value());
        CHECK_FALSE(rowFor("Tile blend", Lane::GpuDevice).has_value());
    }

    // Instrumentation must not be able to change a picture. Same document, same dirty set, so the
    // readback is byte-identical whether or not the timestamps were written.
    common::Image withoutProfiler;
    REQUIRE(lane->readback(common::Rect{}, withoutProfiler, err));
    REQUIRE(withProfiler.width == withoutProfiler.width);
    REQUIRE(withProfiler.height == withoutProfiler.height);
    CHECK(withProfiler.rgba == withoutProfiler.rgba);
}
