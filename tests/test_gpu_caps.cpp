#include "render/gpu_caps.hpp"
#include "render/vulkan_context.hpp"

#include "io/mosaic/docio.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>

using namespace mosaic::render;

namespace {

// A device at Vulkan 1.0's guaranteed floor -- the baseline every case starts from, so a test
// states only what it is actually varying.
GpuProbe floorProbe() {
    GpuProbe p;
    applyFloorProfile(p);
    p.deviceName = "Floor Device";
    p.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    return p;
}

// A generous modern device: 1.3, every optional feature on, big limits.
GpuProbe modernProbe() {
    GpuProbe p = floorProbe();
    p.deviceName = "Modern Device";
    p.deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    p.instanceApiVersion = VK_API_VERSION_1_3;
    p.deviceApiVersion = VK_API_VERSION_1_3;
    p.limits.maxImageDimension2D = 16384;
    p.limits.maxPerStageDescriptorStorageBuffers = 1000000;
    p.limits.maxDescriptorSetStorageBuffers = 1000000;
    p.limits.maxPerStageDescriptorStorageImages = 1000000;
    p.limits.maxDescriptorSetStorageImages = 1000000;
    p.limits.maxPerStageDescriptorSampledImages = 1000000;
    p.limits.maxComputeWorkGroupCount[0] = 65535;
    p.limits.maxComputeWorkGroupCount[1] = 65535;
    p.limits.timestampComputeAndGraphics = VK_TRUE;
    p.limits.timestampPeriod = 1.0f;
    p.timestampValidBits = 64;  // a queue-family fact, not a limit -- see GpuProbe
    p.featStorageBuffer16BitAccess = true;
    p.featShaderFloat16 = true;
    p.featTimelineSemaphore = true;
    p.featRuntimeDescriptorArray = true;
    p.featSampledImageArrayNonUniformIndexing = true;
    p.featDescriptorBindingPartiallyBound = true;
    p.featDescriptorBindingVariableDescriptorCount = true;
    p.featBufferDeviceAddress = true;
    p.featSynchronization2 = true;
    p.featMaintenance4 = true;
    p.subgroupOperations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    p.subgroupSize = 64;
    p.fmtRgba32fLinearFilter = true;
    p.deviceExtensions = {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};
    return p;
}

bool hasExt(const std::vector<const char*>& v, const char* name) {
    return std::any_of(v.begin(), v.end(),
                       [name](const char* e) { return std::string(e) == name; });
}

}  // namespace

TEST_CASE("floor device gets no tiers at all") {
    const GpuCaps caps = decide(floorProbe());

    CHECK(caps.apiVersion == VK_API_VERSION_1_0);
    CHECK_FALSE(caps.subgroupArithmetic);
    CHECK_FALSE(caps.storage16Bit);
    CHECK_FALSE(caps.shaderFloat16);
    CHECK_FALSE(caps.timelineSemaphore);
    CHECK_FALSE(caps.descriptorIndexing);
    CHECK_FALSE(caps.bufferDeviceAddress);
    CHECK_FALSE(caps.synchronization2);
    CHECK_FALSE(caps.maintenance4);
    CHECK_FALSE(caps.hostImageCopy);
    CHECK_FALSE(caps.memoryBudget);
    CHECK_FALSE(caps.timestampQueries);
    CHECK(caps.tierSummary() == "none");
    // dedicatedAllocation is the one flag a bare 1.0 device may still lack; it is extension-only
    // below 1.1 and the floor profile clears the extension list.
    CHECK_FALSE(caps.dedicatedAllocation);
}

TEST_CASE("modern device lights up every tier") {
    const GpuCaps caps = decide(modernProbe());

    CHECK(caps.subgroupArithmetic);
    CHECK(caps.storage16Bit);
    CHECK(caps.shaderFloat16);
    CHECK(caps.timelineSemaphore);
    CHECK(caps.descriptorIndexing);
    CHECK(caps.bufferDeviceAddress);
    CHECK(caps.synchronization2);
    CHECK(caps.maintenance4);
    CHECK(caps.memoryBudget);
    CHECK(caps.dedicatedAllocation);
    CHECK(caps.timestampQueries);
    CHECK_FALSE(caps.hostImageCopy); // 1.4 / the EXT, and this device advertises neither
    CHECK(caps.tierSummary() != "none");
}

TEST_CASE("a version number NEVER implies a capability -- the incomplete-1.3 case") {
    // The exact scenario the capability model exists for: a driver advertising Vulkan 1.3 while
    // gating individual features off. Branching on the version would enable code the device
    // cannot run; branching on the feature bit is a non-event.
    GpuProbe p = modernProbe();
    p.featRuntimeDescriptorArray = false;
    p.featSynchronization2 = false;
    p.featShaderFloat16 = false;

    const GpuCaps caps = decide(p);
    CHECK(caps.apiVersion == VK_API_VERSION_1_3); // still says 1.3 ...
    CHECK_FALSE(caps.descriptorIndexing);         // ... but we do not believe it
    CHECK_FALSE(caps.synchronization2);
    CHECK_FALSE(caps.shaderFloat16);
    CHECK(caps.timelineSemaphore); // untouched features still fire
}

TEST_CASE("descriptor indexing demands all four sub-features") {
    // Partial descriptor-indexing support is common and useless to the tiled compositor: without
    // all four, the per-tile dispatch loop is the correct path.
    for (int missing = 0; missing < 4; ++missing) {
        GpuProbe p = modernProbe();
        switch (missing) {
        case 0: p.featRuntimeDescriptorArray = false; break;
        case 1: p.featSampledImageArrayNonUniformIndexing = false; break;
        case 2: p.featDescriptorBindingPartiallyBound = false; break;
        default: p.featDescriptorBindingVariableDescriptorCount = false; break;
        }
        CAPTURE(missing);
        CHECK_FALSE(decide(p).descriptorIndexing);
    }
    CHECK(decide(modernProbe()).descriptorIndexing);
}

TEST_CASE("an extension substitutes for the core version, and vice versa") {
    // 1.0 device that advertises the pre-promotion extensions: the tier must still fire.
    GpuProbe p = floorProbe();
    p.deviceExtensions = {VK_KHR_16BIT_STORAGE_EXTENSION_NAME,
                          VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
    p.featStorageBuffer16BitAccess = true;
    p.featTimelineSemaphore = true;

    const GpuCaps caps = decide(p);
    CHECK(caps.apiVersion == VK_API_VERSION_1_0);
    CHECK(caps.storage16Bit);
    CHECK(caps.timelineSemaphore);
    // ... and the extension is then REQUESTED at device creation, because it is not core here.
    const auto exts = deviceExtensionsFor(caps, /*needSwapchain=*/false);
    CHECK(hasExt(exts, VK_KHR_16BIT_STORAGE_EXTENSION_NAME));
    CHECK(hasExt(exts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME));
}

TEST_CASE("promoted extensions are not re-requested on a device that has them in core") {
    const GpuCaps caps = decide(modernProbe());
    const auto exts = deviceExtensionsFor(caps, /*needSwapchain=*/true);
    // Core in 1.1/1.2/1.3 respectively -- naming them again is at best noise and at worst an error.
    CHECK_FALSE(hasExt(exts, VK_KHR_16BIT_STORAGE_EXTENSION_NAME));
    CHECK_FALSE(hasExt(exts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME));
    CHECK_FALSE(hasExt(exts, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME));
    CHECK_FALSE(hasExt(exts, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME));
    // Never promoted, so always asked for.
    CHECK(hasExt(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME));
    CHECK(hasExt(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
}

TEST_CASE("portability subset is enabled whenever advertised -- vkCreateDevice fails otherwise") {
    GpuProbe p = floorProbe();
    p.deviceExtensions = {"VK_KHR_portability_subset"};

    const GpuCaps caps = decide(p);
    CHECK(caps.portabilitySubset);
    CHECK(hasExt(deviceExtensionsFor(caps, /*needSwapchain=*/true), "VK_KHR_portability_subset"));
}

TEST_CASE("working format prefers rgba16f even where rgba32f is fully supported") {
    // Settled 2026-07-23: half the memory and bandwidth, and ~11 bits of mantissa clears the
    // 1/255 parity tolerance. fp32 is not chosen merely because it is offered.
    const GpuCaps modern = decide(modernProbe());
    REQUIRE(modernProbe().fmtRgba32fStorage);
    REQUIRE(modernProbe().fmtRgba32fLinearFilter);
    CHECK(modern.workingFormat == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(modern.workingFormatBytes() == 8);
}

TEST_CASE("working format falls back correctly when rgba16f is unusable") {
    SUBCASE("no rgba16f, but rgba32f filters -> fp32") {
        GpuProbe p = modernProbe();
        p.fmtRgba16fStorage = false;
        p.fmtRgba16fLinearFilter = false;
        const GpuCaps caps = decide(p);
        CHECK(caps.workingFormat == VK_FORMAT_R32G32B32A32_SFLOAT);
        CHECK(caps.workingFormatBytes() == 16);
    }
    SUBCASE("rgba16f storage without linear filtering is NOT enough") {
        // We sample working buffers bilinearly (transformed layers, proxies, the loupe), so a
        // storage-only float format cannot serve.
        GpuProbe p = modernProbe();
        p.fmtRgba16fLinearFilter = false;
        CHECK(decide(p).workingFormat == VK_FORMAT_R32G32B32A32_SFLOAT);
    }
    SUBCASE("no usable float format at all -> the rgba8 desperate floor, and it SAYS so") {
        GpuProbe p = floorProbe();
        p.fmtRgba16fStorage = false;
        p.fmtRgba32fStorage = false;
        const GpuCaps caps = decide(p);
        CHECK(caps.workingFormat == VK_FORMAT_R8G8B8A8_UNORM);
        CHECK(caps.workingFormatBytes() == 4);
        CHECK(caps.summary().find("NO float format") != std::string::npos);
    }
}

TEST_CASE("Vulkan 1.0 guarantees rgba32f storage but NOT linear filtering") {
    // The asymmetry is the reason the floor profile is worth running at all: it is the difference
    // between "our bilinear sampling works" and "it works on this developer's GPU".
    GpuProbe p;
    applyFloorProfile(p);
    CHECK(p.fmtRgba32fStorage);
    CHECK_FALSE(p.fmtRgba32fLinearFilter);
    CHECK(p.fmtRgba16fStorage);
    CHECK(p.fmtRgba16fLinearFilter);
    CHECK(decide(p).workingFormat == VK_FORMAT_R16G16B16A16_SFLOAT);
}

TEST_CASE("the dirty-tile grid is the .mosaic store's tile grid") {
    // The load-bearing alignment (docs/s60-performance-plan.md section 3): one dirty set feeds
    // both the recomposite and the autosave journal, and an in-memory tile maps 1:1 onto a
    // stored tile with no re-tiling on save. If either constant moves, this fails loudly.
    CHECK(kDirtyTileSize == mosaic::io::native::kTileSize);
}

TEST_CASE("macrotile is a multiple of the dirty tile and fits in one image") {
    SUBCASE("default k on real hardware") {
        const GpuCaps caps = decide(modernProbe());
        CHECK(caps.macrotileSize == kDirtyTileSize << kMacrotileDefaultShift);
        CHECK(caps.macrotileSize == 256);
        CHECK(caps.macrotileSize % kDirtyTileSize == 0);
    }
    SUBCASE("a software device gets a smaller granule") {
        GpuProbe p = modernProbe();
        p.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
        const GpuCaps caps = decide(p);
        CHECK(caps.softwareDevice);
        CHECK(caps.macrotileSize == kDirtyTileSize << kMacrotileSoftwareShift);
        CHECK(caps.macrotileSize == 128);
    }
    SUBCASE("a tiny maxImageDimension2D shrinks the granule rather than producing an illegal one") {
        GpuProbe p = floorProbe();
        p.limits.maxImageDimension2D = 128;
        const GpuCaps caps = decide(p);
        CHECK(caps.macrotileSize <= caps.maxImageDim);
        CHECK(caps.macrotileSize % kDirtyTileSize == 0);
        CHECK(caps.macrotileSize >= kDirtyTileSize);
    }
    SUBCASE("even an absurd 64px image limit still yields a legal macrotile") {
        GpuProbe p = floorProbe();
        p.limits.maxImageDimension2D = 64;
        const GpuCaps caps = decide(p);
        CHECK(caps.macrotileSize == kDirtyTileSize);
    }
}

TEST_CASE("lane admission: a lane must ask, never assume") {
    SUBCASE("extrude_raster's 7 storage buffers do NOT fit the 1.0 floor") {
        // The live case: shaders/extrude_raster.comp binds 7 SSBOs against a guaranteed 4.
        const GpuCaps floor = decide(floorProbe());
        CHECK(floor.limits.maxPerStageDescriptorStorageBuffers == 4);
        CHECK_FALSE(floor.fitsStorageBuffers(7));
        CHECK(floor.fitsStorageBuffers(4));
        CHECK(decide(modernProbe()).fitsStorageBuffers(7));
    }
    SUBCASE("a document larger than maxImageDimension2D cannot be one image") {
        // Correctness, not perf: at the 1.0 floor a 5000x8000 document must be tiled.
        const GpuCaps floor = decide(floorProbe());
        CHECK(floor.maxImageDim == 4096);
        CHECK_FALSE(floor.fitsImage(5000, 8000));
        CHECK(floor.fitsImage(4096, 4096));
        CHECK_FALSE(floor.fitsImage(0, 100)); // degenerate sizes are never "fitting"
        CHECK(decide(modernProbe()).fitsImage(5000, 8000));
    }
    SUBCASE("canvas_present.comp's push block sits exactly on the 1.0 budget") {
        const GpuCaps floor = decide(floorProbe());
        CHECK(floor.fitsPushConstants(128));
        CHECK_FALSE(floor.fitsPushConstants(129));
    }
    SUBCASE("storage/sampled image counts") {
        const GpuCaps floor = decide(floorProbe());
        CHECK(floor.fitsStorageImages(4));
        CHECK_FALSE(floor.fitsStorageImages(5));
        CHECK(floor.fitsSampledImages(16));
        CHECK_FALSE(floor.fitsSampledImages(17));
    }
}

TEST_CASE("the usable version is the LOWER of instance and device") {
    // A 1.3 device behind a 1.0 loader is a 1.0 device for this process.
    GpuProbe p = modernProbe();
    p.instanceApiVersion = VK_API_VERSION_1_0;
    const GpuCaps caps = decide(p);
    CHECK(caps.apiVersion == VK_API_VERSION_1_0);
    // Features the driver reports are still honoured -- they came with extensions, not a version.
    // But the ones gated purely on core promotion must not fire.
    CHECK_FALSE(caps.synchronization2);
    CHECK_FALSE(caps.maintenance4);
}

TEST_CASE("subgroup arithmetic needs both the bit and a reported size") {
    GpuProbe p = modernProbe();
    SUBCASE("arithmetic bit missing") {
        p.subgroupOperations = VK_SUBGROUP_FEATURE_BASIC_BIT;
        CHECK_FALSE(decide(p).subgroupArithmetic);
    }
    SUBCASE("size unreported") {
        p.subgroupSize = 0;
        CHECK_FALSE(decide(p).subgroupArithmetic);
    }
}

TEST_CASE("timestamp queries need a usable period, not just the flag") {
    GpuProbe p = modernProbe();
    p.limits.timestampPeriod = 0.0f;
    CHECK_FALSE(decide(p).timestampQueries);
    // And the counter width goes with it, so render::GpuTimer cannot pick up a stray mask from a
    // device it was refused on.
    CHECK(decide(p).timestampValidBits == 0);
}

TEST_CASE("timestamp queries also need a queue family that can WRITE one") {
    // The limit is a device-wide promise; timestampValidBits is per queue family, and a family
    // reporting 0 cannot execute vkCmdWriteTimestamp at all. Both must hold, which is why the
    // width is probed from VkQueueFamilyProperties alongside the transfer/async-compute families.
    GpuProbe p = modernProbe();
    p.timestampValidBits = 0;
    CHECK_FALSE(decide(p).timestampQueries);

    p.timestampValidBits = 36;  // Vulkan 1.0's guaranteed minimum where the limit is set
    const GpuCaps caps = decide(p);
    CHECK(caps.timestampQueries);
    CHECK(caps.timestampValidBits == 36);
}

TEST_CASE("summary reports the device, version, format and granule") {
    const std::string s = decide(modernProbe()).summary();
    CHECK(s.find("Modern Device") != std::string::npos);
    CHECK(s.find("Vulkan 1.3") != std::string::npos);
    CHECK(s.find("rgba16f") != std::string::npos);
    CHECK(s.find("256px") != std::string::npos);

    GpuProbe soft = modernProbe();
    soft.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
    CHECK(decide(soft).summary().find("(software)") != std::string::npos);
}

TEST_CASE("the feature chain links exactly what caps asked for") {
    SUBCASE("a floor device requests no features at all") {
        const GpuFeatureChain chain{decide(floorProbe())};
        CHECK(chain.pNext() == nullptr);
    }
    SUBCASE("a modern device builds a chain, and enables no core 1.0 feature") {
        const GpuCaps caps = decide(modernProbe());
        const GpuFeatureChain chain{caps};
        REQUIRE(chain.pNext() != nullptr);

        // Walk the chain and confirm every linked struct is one caps actually enabled.
        int count = 0;
        const auto* p = static_cast<const VkBaseInStructure*>(chain.pNext());
        while (p != nullptr) {
            ++count;
            CHECK(p->sType != 0);
            p = p->pNext;
        }
        // storage16, float16, timeline, descriptor-indexing, buffer-address, sync2, maintenance4.
        CHECK(count == 7);

        // Mosaic has never needed a core 1.0 feature; enabling one we do not use would only
        // narrow the set of devices that accept us.
        const VkPhysicalDeviceFeatures& f = chain.coreFeatures();
        CHECK(f.samplerAnisotropy == VK_FALSE);
        CHECK(f.shaderFloat64 == VK_FALSE);
        CHECK(f.shaderInt64 == VK_FALSE);
        CHECK(f.fragmentStoresAndAtomics == VK_FALSE);
    }
}

TEST_CASE("applyFloorProfile clamps a generous device all the way down") {
    GpuProbe p = modernProbe();
    applyFloorProfile(p);
    const GpuCaps caps = decide(p);

    CHECK(caps.apiVersion == VK_API_VERSION_1_0);
    CHECK(caps.maxImageDim == 4096);
    CHECK(caps.tierSummary() == "none");
    CHECK_FALSE(caps.fitsStorageBuffers(7));
    CHECK_FALSE(caps.fitsImage(5000, 8000));
    // The device TYPE is not a limit, so it survives -- the profile clamps capability, not identity.
    CHECK(caps.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    CHECK(caps.deviceName == "Modern Device");
}

TEST_CASE("floorLimits is a legal minimal Vulkan 1.0 limit set") {
    const VkPhysicalDeviceLimits l = floorLimits();
    CHECK(l.maxImageDimension2D == vk10::kMaxImageDimension2D);
    CHECK(l.maxComputeWorkGroupInvocations == vk10::kMaxComputeWorkGroupInvocations);
    CHECK(l.maxPushConstantsSize == vk10::kMaxPushConstantsSize);
    CHECK(l.maxPerStageDescriptorStorageBuffers == vk10::kMaxPerStageDescriptorStorageBuffers);
    CHECK(l.maxComputeSharedMemorySize == vk10::kMaxComputeSharedMemorySize);
    CHECK(l.maxBoundDescriptorSets == vk10::kMaxBoundDescriptorSets);

    // Every shader in the tree is 8x8x1 = 64 invocations, deliberately under the 128 floor.
    CHECK(8u * 8u * 1u <= l.maxComputeWorkGroupInvocations);
    CHECK(64u <= l.maxComputeWorkGroupInvocations); // extrude_raster.comp is 64x1x1

    const GpuCaps caps = decide(floorProbe());
    CHECK(caps.maxWorkgroupsPerAxis() == vk10::kMaxComputeWorkGroupCount);
}

// ---------------------------------------------------------------------------------------------
// Shared device (S60-alpha, docs/s60-performance-plan.md section 1.2). These need a real Vulkan
// device and skip cleanly without one, like the other GPU-lane tests.
// ---------------------------------------------------------------------------------------------

TEST_CASE("VulkanContext::shared hands every borrower the SAME device") {
    std::string err;
    auto a = mosaic::render::VulkanContext::shared(/*enableValidation=*/false, err);
    if (!a)
        return; // no device here (CI / headless without a GPU) -- nothing to assert
    auto b = mosaic::render::VulkanContext::shared(/*enableValidation=*/false, err);
    auto c = mosaic::render::VulkanContext::shared(/*enableValidation=*/false, err);
    REQUIRE(b);
    REQUIRE(c);

    // The point of the exercise: Mosaic used to stand up one device PER LANE.
    CHECK(a.get() == b.get());
    CHECK(b.get() == c.get());
    CHECK(a->device() == b->device());
    CHECK(a->instance() == c->instance());
    CHECK(a->physicalDevice() == c->physicalDevice());
    CHECK(a->queue() == c->queue());
    // ... and one capability probe serves all of them.
    CHECK(a->caps().deviceName == c->caps().deviceName);
}

TEST_CASE("each borrower gets its OWN command pool") {
    // Command pools are externally synchronized, so lanes sharing a device must not share a pool.
    std::string err;
    auto ctx = mosaic::render::VulkanContext::shared(/*enableValidation=*/false, err);
    if (!ctx)
        return;

    const VkCommandPool p1 = ctx->createCommandPool(err);
    const VkCommandPool p2 = ctx->createCommandPool(err);
    REQUIRE(p1 != VK_NULL_HANDLE);
    REQUIRE(p2 != VK_NULL_HANDLE);
    CHECK(p1 != p2);
    CHECK(p1 != ctx->commandPool()); // ... and neither is the context's own
    CHECK(p2 != ctx->commandPool());
    vkDestroyCommandPool(ctx->device(), p1, nullptr);
    vkDestroyCommandPool(ctx->device(), p2, nullptr);
}

TEST_CASE("the shared device outlives its borrowers, in any drop order") {
    // Lanes are function-local statics in the app; their destruction order is not something this
    // code gets to choose. Holding the device by shared_ptr makes the ordering irrelevant by
    // construction -- the device cannot be torn down while any lane still refers to it.
    std::string err;
    auto first = mosaic::render::VulkanContext::shared(/*enableValidation=*/false, err);
    if (!first)
        return;
    const VkDevice dev = first->device();
    {
        auto second = mosaic::render::VulkanContext::shared(/*enableValidation=*/false, err);
        REQUIRE(second);
        first.reset(); // the "first" borrower goes away while the second is still working
        CHECK(second->device() == dev);
        CHECK(vkDeviceWaitIdle(second->device()) == VK_SUCCESS); // still a live device
    }
    // Everyone has let go; the next borrower may legitimately get a freshly built device.
    auto third = mosaic::render::VulkanContext::shared(/*enableValidation=*/false, err);
    REQUIRE(third);
    CHECK(third->device() != VK_NULL_HANDLE);
}
