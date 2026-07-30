#include "render/gpu_budget.hpp"

#include "render/gpu_caps.hpp"

#include <doctest/doctest.h>

using namespace mosaic::render;

namespace {

GpuMemoryBudget snapshot(std::uint64_t budgetBytes, std::uint64_t usageBytes, bool measured) {
    GpuMemoryBudget m;
    m.heapSize = budgetBytes;
    m.budget = budgetBytes;
    m.usage = usageBytes;
    m.measured = measured;
    return m;
}

GpuCaps discreteCaps() {
    GpuProbe p;
    applyFloorProfile(p);
    p.deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    return decide(p);
}

GpuCaps softwareCaps() {
    GpuProbe p;
    applyFloorProfile(p);
    p.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
    return decide(p);
}

constexpr std::uint64_t kMiB = 1024ull * 1024ull;

}  // namespace

TEST_CASE("available() is budget minus usage, floored at zero") {
    CHECK(snapshot(1000, 400, true).available() == 600);
    CHECK(snapshot(1000, 1000, true).available() == 0);
    // A driver may report usage ABOVE budget (another process took the heap). That is 0 available,
    // not a colossal number from unsigned wraparound -- which would have us allocate into a full
    // heap and lose the device.
    CHECK(snapshot(1000, 4000, true).available() == 0);
}

TEST_CASE("an ESTIMATED figure is spent more cautiously than a measured one") {
    const GpuCaps caps = discreteCaps();
    const std::uint64_t measured = atlasBudgetBytes(snapshot(2048 * kMiB, 0, true), caps);
    const std::uint64_t guessed = atlasBudgetBytes(snapshot(2048 * kMiB, 0, false), caps);
    // Same nominal heap, but one number was observed and the other assumed. On a shared desktop
    // GPU the gap between heap size and what is actually free is routinely a gigabyte.
    CHECK(guessed < measured);
}

TEST_CASE("the atlas takes a share of what is free, not of the whole heap") {
    const GpuCaps caps = discreteCaps();
    // 8 GiB heap with 7 GiB already taken by other processes: budget against the 1 GiB that is
    // left, never against the 8.
    const std::uint64_t busy = atlasBudgetBytes(snapshot(8192 * kMiB, 7168 * kMiB, true), caps);
    const std::uint64_t idle = atlasBudgetBytes(snapshot(8192 * kMiB, 0, true), caps);
    CHECK(busy < idle);
    CHECK(busy <= 1024 * kMiB);
}

TEST_CASE("the hard cap bounds a very large heap") {
    const GpuCaps caps = discreteCaps();
    // 60% of 24 GiB would be absurd for a tile atlas; the cap is what stops a percentage policy
    // from scaling into nonsense on a workstation card.
    CHECK(atlasBudgetBytes(snapshot(24576 * kMiB, 0, true), caps) == 1024 * kMiB);
    CHECK(atlasBudgetBytes(snapshot(24576 * kMiB, 0, true), caps, 0.60, 256 * kMiB) == 256 * kMiB);
}

TEST_CASE("a squeezed device still gets a working atlas, never a useless one") {
    const GpuCaps caps = discreteCaps();
    // Almost nothing free. An atlas that can hold two tiles would thrash every frame, so the
    // floor wins -- and if even the floor will not fit, the caller's answer is the CPU path, not
    // a limping GPU one.
    const std::uint64_t squeezed = atlasBudgetBytes(snapshot(512 * kMiB, 511 * kMiB, true), caps);
    CHECK(squeezed == 32 * kMiB);
    CHECK(atlasBudgetBytes(snapshot(0, 0, true), caps) == 32 * kMiB);
}

TEST_CASE("a software device holds far less -- its 'GPU memory' is the user's RAM") {
    // lavapipe reserving a gigabyte of system RAM serves no purpose: the atlas exists to keep
    // pixels off the PCIe bus, and there is no bus.
    const std::uint64_t soft = atlasBudgetBytes(snapshot(16384 * kMiB, 0, true), softwareCaps());
    const std::uint64_t hard = atlasBudgetBytes(snapshot(16384 * kMiB, 0, true), discreteCaps());
    CHECK(soft < hard);
    CHECK(soft <= 128 * kMiB);
}

TEST_CASE("the headroom fraction is clamped, so a bad caller cannot over-commit") {
    const GpuCaps caps = discreteCaps();
    const GpuMemoryBudget m = snapshot(2048 * kMiB, 0, true);
    CHECK(atlasBudgetBytes(m, caps, 5.0) <= 1024 * kMiB);   // >100% clamps, then the cap bounds it
    CHECK(atlasBudgetBytes(m, caps, -1.0) == 32 * kMiB);    // negative clamps to 0 -> the floor
}

TEST_CASE("summary states whether the numbers were measured or assumed") {
    // "we measured 2 GB free" and "we assumed 2 GB free" must not read the same in a log.
    CHECK(snapshot(2048 * kMiB, 0, true).summary().find("measured") != std::string::npos);
    CHECK(snapshot(2048 * kMiB, 0, false).summary().find("ESTIMATED") != std::string::npos);
}
