#include "render/gpu_budget.hpp"

#include "render/vulkan_context.hpp"

#include <algorithm>
#include <vector>

namespace mosaic::render {
namespace {

// Bytes -> a short human string. Diagnostics only; exactness is not the point.
std::string mib(std::uint64_t bytes) {
    return std::to_string(bytes / (1024ull * 1024ull)) + " MiB";
}

}  // namespace

std::string GpuMemoryBudget::summary() const {
    std::string s = "heap " + std::to_string(heapIndex) + ": " + mib(available()) + " available of " +
                    mib(budget);
    s += measured ? " (measured)" : " (ESTIMATED -- VK_EXT_memory_budget absent)";
    if (unifiedMemory)
        s += ", unified";
    return s;
}

GpuMemoryBudget queryGpuMemory(const VulkanContext& ctx) {
    GpuMemoryBudget out;
    const VkPhysicalDevice dev = ctx.physicalDevice();
    if (dev == VK_NULL_HANDLE)
        return out;

    // The budget struct chains onto memoryProperties2, which needs either a 1.1 instance or
    // VK_KHR_get_physical_device_properties2 -- the same availability rule as the capability
    // probe. GpuCaps::memoryBudget already encodes whether the extension was enabled at device
    // creation, so trust that rather than re-deriving it.
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProps{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    const bool wantMeasured = ctx.caps().memoryBudget;
    if (wantMeasured)
        props2.pNext = &budgetProps;

    auto getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
        vkGetInstanceProcAddr(ctx.instance(), "vkGetPhysicalDeviceMemoryProperties2"));
    if (getProps2 == nullptr)
        getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
            vkGetInstanceProcAddr(ctx.instance(), "vkGetPhysicalDeviceMemoryProperties2KHR"));

    VkPhysicalDeviceMemoryProperties mp{};
    if (getProps2 != nullptr) {
        getProps2(dev, &props2);
        mp = props2.memoryProperties;
    } else {
        vkGetPhysicalDeviceMemoryProperties(dev, &mp);
    }

    // Pick the largest DEVICE_LOCAL heap: the working set belongs where the compute reads it.
    std::uint32_t best = UINT32_MAX;
    for (std::uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
        if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
            continue;
        if (best == UINT32_MAX || mp.memoryHeaps[i].size > mp.memoryHeaps[best].size)
            best = i;
    }
    if (best == UINT32_MAX) { // no device-local heap at all (lavapipe and friends): heap 0 it is
        if (mp.memoryHeapCount == 0)
            return out;
        best = 0;
    }
    out.heapIndex = best;
    out.heapSize = mp.memoryHeaps[best].size;

    // Unified when some memory type in this heap is BOTH device-local and host-visible: an
    // integrated GPU, or a discrete one with resizable BAR. Uploads there can skip staging.
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const auto& t = mp.memoryTypes[i];
        if (t.heapIndex != best)
            continue;
        constexpr VkMemoryPropertyFlags kBoth =
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        if ((t.propertyFlags & kBoth) == kBoth) {
            out.unifiedMemory = true;
            break;
        }
    }

    if (wantMeasured && getProps2 != nullptr) {
        out.budget = budgetProps.heapBudget[best];
        out.usage = budgetProps.heapUsage[best];
        out.measured = out.budget != 0; // a driver reporting 0 has told us nothing
    }
    if (!out.measured) {
        // No extension (or a useless answer): assume the heap is ours and let atlasBudgetFor's
        // headroom fraction carry the conservatism. Saying `usage = 0` here is a fiction, and
        // `measured == false` is how the caller learns that.
        out.budget = out.heapSize;
        out.usage = 0;
    }
    return out;
}

std::uint64_t atlasBudgetBytes(const GpuMemoryBudget& mem, const GpuCaps& caps,
                               double headroomFraction, std::uint64_t hardCapBytes,
                               std::uint64_t minBytes) {
    const double frac = std::clamp(headroomFraction, 0.0, 1.0);
    std::uint64_t avail = mem.available();

    // An ESTIMATED figure is a claim about a number nobody measured, so halve it before spending
    // against it: on a shared desktop GPU the difference between heap size and what is actually
    // free is routinely a gigabyte or more.
    if (!mem.measured)
        avail /= 2;

    // A software device's "GPU memory" is system RAM, and the whole point of the atlas -- keeping
    // pixels off the PCIe bus -- does not apply. Hold far less; lavapipe would otherwise reserve
    // a gigabyte of the user's RAM to no purpose.
    if (caps.softwareDevice)
        hardCapBytes = std::min<std::uint64_t>(hardCapBytes, 128ull << 20);

    auto budget = static_cast<std::uint64_t>(static_cast<double>(avail) * frac);
    budget = std::min(budget, hardCapBytes);
    return std::max(budget, minBytes);
}

}  // namespace mosaic::render
