#pragma once

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

#include "render/gpu_caps.hpp"

// GPU memory budgeting for the resident compositor (S60-a/-d, docs/s60-performance-plan.md §2.2,
// §3.3).
//
// The tile atlas has to decide how much device memory it may hold and when to evict, and both
// answers are worthless if they are guesses. `VK_EXT_memory_budget` is the only portable way to
// ask what is actually free RIGHT NOW -- heap SIZE is a hardware constant, but heap USAGE moves
// with every other process on the machine (a browser, a compositor, another Vulkan app). Budgeting
// against heap size on a shared desktop GPU is how an editor ends up thrashing or losing the
// device.
//
// The extension is not guaranteed, so this degrades in a stated way rather than silently: without
// it we fall back to a conservative fraction of the largest device-local heap and SAY SO, because
// "we measured 2 GB free" and "we assumed 2 GB free" must not look the same to the caller.

namespace mosaic::render {

class VulkanContext;

// What the device says about one snapshot in time. All figures in bytes.
struct GpuMemoryBudget {
    // The heap the working set should live in -- the largest DEVICE_LOCAL one.
    std::uint32_t heapIndex = 0;
    std::uint64_t heapSize = 0;   // the hardware constant
    std::uint64_t budget = 0;     // what this process may use (== heapSize when estimated)
    std::uint64_t usage = 0;      // what this process is currently using
    // True when the numbers came from VK_EXT_memory_budget; false when they are an estimate from
    // heap sizes. A caller that cares (an eviction policy does) can be more conservative.
    bool measured = false;
    // Device-local memory that is also HOST_VISIBLE covers the whole heap: an integrated GPU, or
    // a discrete one in resizable-BAR mode. Uploads there can skip staging entirely.
    bool unifiedMemory = false;

    // Bytes still available to this process, floored at 0.
    [[nodiscard]] std::uint64_t available() const noexcept {
        return budget > usage ? budget - usage : 0;
    }
    [[nodiscard]] std::string summary() const;
};

// Snapshot the device's memory state. Cheap enough to call per eviction pass, NOT per tile --
// vkGetPhysicalDeviceMemoryProperties2 walks every heap and type.
[[nodiscard]] GpuMemoryBudget queryGpuMemory(const VulkanContext& ctx);

// How many bytes the tile atlas may hold, given a snapshot.
//
// Deliberately not "all of it". Three claims share this heap and only one of them is the atlas:
// the swapchain and the present pass, the other GPU lanes (blur, texture, 3D text), and every
// other process on a desktop GPU. `headroomFraction` is the share of *available* memory the atlas
// may take, and `hardCapBytes` bounds it on a machine with a very large heap where a percentage
// would be absurd. The result is floored at `minBytes` so a squeezed device still gets a working
// atlas rather than one that can hold nothing and thrashes every frame -- if even that does not
// fit, the caller should fall back to the CPU path rather than limp.
//
// Pure: no Vulkan calls, so the policy is unit-testable against synthetic snapshots.
[[nodiscard]] std::uint64_t atlasBudgetBytes(const GpuMemoryBudget& mem, const GpuCaps& caps,
                                             double headroomFraction = 0.60,
                                             std::uint64_t hardCapBytes = 1ull << 30,
                                             std::uint64_t minBytes = 32ull << 20);

}  // namespace mosaic::render
