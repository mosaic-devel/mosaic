#include "render/extrude_gpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "core/text/extrude_render.hpp"  // ExtrudeCamera (shared derivation) + tile composite
#include "render/gpu_policy.hpp"
#include "render/vulkan_context.hpp"
#include <shaders/extrude_raster.comp.spv.hpp>

// The Vulkan compute-rasterizer lane for extruded text (S30-c). Uploads are memcpys through
// persistently-mapped host-visible buffers; the WORKING buffers (depth + color) are DEVICE-LOCAL
// -- the shade pass hammers them per sample, and host-visible memory put every one of those
// accesses on the PCIe bus. The tile comes back through an explicit copy into a HOST_CACHED
// staging buffer: reading a mapped write-combined allocation directly was the original sin --
// uncached CPU reads made the readback alone slower than the whole CPU lane (drag-lag feedback
// 2026-07-03; the bench in test_extrude_gpu.cpp is the receipt). See shaders/extrude_raster.comp
// for the pass structure.
namespace mosaic::render {
namespace {

using core::text::Extrude;
using core::text::ExtrudeCamera;
using core::text::ExtrudeMesh;
using core::text::Material;

constexpr VkDeviceSize kMaxTileBytes = 256ull << 20;  // refuse absurd tiles; CPU lane handles them

// One host-visible SSBO with capacity semantics (grow-only, persistently mapped).
struct HostBuffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;

    void destroy(VkDevice dev) {
        if (mapped != nullptr) vkUnmapMemory(dev, mem);
        if (buf != VK_NULL_HANDLE) vkDestroyBuffer(dev, buf, nullptr);
        if (mem != VK_NULL_HANDLE) vkFreeMemory(dev, mem, nullptr);
        buf = VK_NULL_HANDLE;
        mem = VK_NULL_HANDLE;
        mapped = nullptr;
        capacity = 0;
    }
};

// FNV-1a over the mesh's geometry: the buffer-cache key (§10.5 -- geometry edits re-upload,
// rotate/light/material edits do not).
std::uint64_t meshHash(const ExtrudeMesh& mesh) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](const void* data, std::size_t bytes) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    mix(mesh.vertices.data(), mesh.vertices.size() * sizeof(mesh.vertices[0]));
    mix(mesh.indices.data(), mesh.indices.size() * sizeof(mesh.indices[0]));
    mix(mesh.ranges.data(), mesh.ranges.size() * sizeof(mesh.ranges[0]));
    return h;
}

// The std430 mirror of the shader's Params block (see extrude_raster.comp binding 6).
struct GpuParams {
    float rot[16];         // column-major mat4
    float camCenterDist[4];
    float pixelLin[4];
    float pixelOff[4];
    std::int32_t tile[4];
    float ambient[4];
    std::int32_t counts[4];
    float lightDir[8][4];
    float lightCol[8][4];
    float envLin[4];   // ExtrudeEnv::layerToEnv linear
    float envOff[4];   // layerToEnv tx ty, env width, env height
    float envMisc[4];  // zEnv, reflect-active flag, reflect-sides-only, overlay wrap-sides
};

// Per-slot texel offsets must stay exactly representable in the float the mats row carries them
// in; far beyond any real overlay upload (the builder caps a map at 2M texels).
constexpr std::uint64_t kMaxOverlayTexels = 16'000'000;

}  // namespace

struct ExtrudeGpu::Impl {
    std::shared_ptr<VulkanContext> ctx;  // the process-wide shared device (S60-alpha)
    VkCommandPool pool = VK_NULL_HANDLE; // OURS: pools are externally synchronized
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    HostBuffer verts, indices, triMat;   // mesh-hash cached (host-visible uploads)
    HostBuffer mats, paramsBuf;          // per-render uploads (host-visible)
    HostBuffer envBuf;                   // the reflection snapshot (pointer-identity cached)
    HostBuffer overlayBuf;               // the S30-e overlay maps (per-render, no caching --
                                         // a render only happens on a text-cache refresh)
    HostBuffer depth, color;             // working buffers (device-local, never mapped)
    HostBuffer staging;                  // the readback tile (host-visible, prefer cached)
    std::uint64_t cachedHash = 0;
    std::uint32_t cachedTris = 0;
    const void* cachedEnvData = nullptr;  // env re-upload key (a rebuilt snapshot reallocates)
    std::size_t cachedEnvBytes = 0;

    ~Impl() {
        if (!ctx) return;
        const VkDevice dev = ctx->device();
        ctx->waitIdle();
        for (HostBuffer* b : {&verts, &indices, &triMat, &mats, &paramsBuf, &envBuf, &overlayBuf,
                              &depth, &color, &staging})
            b->destroy(dev);
        if (fence != VK_NULL_HANDLE) vkDestroyFence(dev, fence, nullptr);
        if (descPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, descPool, nullptr);
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipeline, nullptr);
        if (shader != VK_NULL_HANDLE) vkDestroyShaderModule(dev, shader, nullptr);
        if (pipeLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
        if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
        if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(dev, pool, nullptr);
    }

    // Kind decides where the allocation lives: uploads stay host-visible + persistently mapped;
    // the working buffers want device-local (falling back to host-visible where none exists --
    // correct, just slow); the readback staging wants HOST_CACHED so the CPU composite reads at
    // cache speed (write-combined-only memory serves as the fallback).
    enum class Kind { Upload, DeviceLocal, Readback };
    bool ensureBuffer(HostBuffer& b, VkDeviceSize bytes, VkBufferUsageFlags usage, Kind kind) {
        if (b.capacity >= bytes && b.buf != VK_NULL_HANDLE) return true;
        const VkDevice dev = ctx->device();
        b.destroy(dev);
        const VkBufferCreateInfo bci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = std::max<VkDeviceSize>(bytes, 256),
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (vkCreateBuffer(dev, &bci, nullptr, &b.buf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(dev, b.buf, &mr);
        constexpr VkMemoryPropertyFlags kHost =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::uint32_t type = UINT32_MAX;
        if (kind == Kind::DeviceLocal)
            type = ctx->findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        else if (kind == Kind::Readback)
            type = ctx->findMemoryType(mr.memoryTypeBits,
                                       kHost | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type == UINT32_MAX) type = ctx->findMemoryType(mr.memoryTypeBits, kHost);
        if (type == UINT32_MAX) return false;
        const VkMemoryAllocateInfo mai{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = type,
        };
        if (vkAllocateMemory(dev, &mai, nullptr, &b.mem) != VK_SUCCESS ||
            vkBindBufferMemory(dev, b.buf, b.mem, 0) != VK_SUCCESS)
            return false;
        b.mapped = nullptr;
        if (kind != Kind::DeviceLocal &&
            vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped) != VK_SUCCESS)
            return false;
        b.capacity = bci.size;
        return true;
    }

    // extrude_raster.comp binds NINE storage buffers (verts, indices, tri-materials, materials,
    // depth, colour, params, reflection env, overlay maps). Vulkan 1.0 guarantees only FOUR per
    // stage, so this lane does not fit a floor device -- it must ASK, not assume, and hand the
    // work back to the CPU lane when the answer is no (S60-alpha).
    static constexpr std::uint32_t kStorageBufferBindings = 9;

    bool init(bool enableValidation, std::string& error) {
        ctx = VulkanContext::shared(enableValidation, error);
        if (!ctx) return false;
        pool = ctx->createCommandPool(error);
        if (pool == VK_NULL_HANDLE) return false;
        if (!ctx->caps().fitsStorageBuffers(kStorageBufferBindings)) {
            error = "device allows only " +
                    std::to_string(ctx->caps().limits.maxPerStageDescriptorStorageBuffers) +
                    " storage buffers per stage; this lane needs " +
                    std::to_string(kStorageBufferBindings);
            return false;
        }
        const VkDevice dev = ctx->device();

        VkDescriptorSetLayoutBinding bindings[kStorageBufferBindings]{};
        for (std::uint32_t i = 0; i < kStorageBufferBindings; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        const VkDescriptorSetLayoutCreateInfo slci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = kStorageBufferBindings,
            .pBindings = bindings,
        };
        if (vkCreateDescriptorSetLayout(dev, &slci, nullptr, &setLayout) != VK_SUCCESS) {
            error = "descriptor set layout creation failed";
            return false;
        }
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t)};
        const VkPipelineLayoutCreateInfo plci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &setLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        };
        if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipeLayout) != VK_SUCCESS) {
            error = "pipeline layout creation failed";
            return false;
        }
        const VkShaderModuleCreateInfo smci{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shaders::extrude_raster_comp_size,
            .pCode = shaders::extrude_raster_comp,
        };
        if (vkCreateShaderModule(dev, &smci, nullptr, &shader) != VK_SUCCESS) {
            error = "shader module creation failed";
            return false;
        }
        const VkComputePipelineCreateInfo cpci{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = shader,
                      .pName = "main"},
            .layout = pipeLayout,
        };
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline) !=
            VK_SUCCESS) {
            error = "compute pipeline creation failed";
            return false;
        }
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            kStorageBufferBindings};
        const VkDescriptorPoolCreateInfo dpci{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize,
        };
        if (vkCreateDescriptorPool(dev, &dpci, nullptr, &descPool) != VK_SUCCESS) {
            error = "descriptor pool creation failed";
            return false;
        }
        const VkDescriptorSetAllocateInfo dsai{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = descPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &setLayout,
        };
        if (vkAllocateDescriptorSets(dev, &dsai, &descSet) != VK_SUCCESS) {
            error = "descriptor set allocation failed";
            return false;
        }
        const VkCommandBufferAllocateInfo cbai{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(dev, &cbai, &cmd) != VK_SUCCESS) {
            error = "command buffer allocation failed";
            return false;
        }
        const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) {
            error = "fence creation failed";
            return false;
        }
        return true;
    }
};

ExtrudeGpu::ExtrudeGpu() : m_impl(std::make_unique<Impl>()) {}
ExtrudeGpu::~ExtrudeGpu() = default;

std::unique_ptr<ExtrudeGpu> ExtrudeGpu::create(bool enableValidation, std::string& error) {
    // CPU-only mode (render/gpu_policy.hpp, S60-b item 14). The fallback is the CPU tile
    // rasterizer in core/text/extrude_render.cpp, which shares this lane's camera derivation --
    // so the refusal costs speed and nothing else.
    if (!computeLaneAllowed("extrude", error)) return nullptr;
    auto gpu = std::unique_ptr<ExtrudeGpu>(new ExtrudeGpu());
    if (!gpu->m_impl->init(enableValidation, error)) return nullptr;
    return gpu;
}

bool ExtrudeGpu::render(common::ImageF& dst, const ExtrudeMesh& mesh, const Extrude& params,
                        const common::Affine2D& toPixel, bool antialias,
                        const core::text::ExtrudeEnv* env,
                        const core::text::ExtrudeOverlay* overlay) {
    Impl& im = *m_impl;
    if (!im.ctx || mesh.empty() || dst.width == 0 || dst.height == 0) return false;
    if (overlay != nullptr && overlay->empty()) overlay = nullptr;
    const VkDevice dev = im.ctx->device();
    const int S = antialias ? 2 : 1;

    // --- The tile: same camera + AABB derivation as the CPU lane (parity by construction) ------
    const ExtrudeCamera cam = ExtrudeCamera::from(mesh.designBounds, params);
    double minPx = std::numeric_limits<double>::infinity(), minPy = minPx;
    double maxPx = -minPx, maxPy = -minPy;
    for (const auto& v : mesh.vertices) {
        double depth = 0.0;
        const common::Vec2 design = cam.project(cam.rotate(params, v.position), depth);
        const common::Vec2 dev2 = toPixel.apply(design);
        minPx = std::min(minPx, dev2.x * S);
        minPy = std::min(minPy, dev2.y * S);
        maxPx = std::max(maxPx, dev2.x * S);
        maxPy = std::max(maxPy, dev2.y * S);
    }
    const long tx0 = std::max(0L, static_cast<long>(std::floor(minPx)) - 1);
    const long ty0 = std::max(0L, static_cast<long>(std::floor(minPy)) - 1);
    const long tx1 = std::min(static_cast<long>(dst.width) * S,
                              static_cast<long>(std::ceil(maxPx)) + 1);
    const long ty1 = std::min(static_cast<long>(dst.height) * S,
                              static_cast<long>(std::ceil(maxPy)) + 1);
    if (tx1 <= tx0 || ty1 <= ty0) return true;  // nothing visible: handled (nothing to draw)
    const std::uint64_t tw = static_cast<std::uint64_t>(tx1 - tx0);
    const std::uint64_t th = static_cast<std::uint64_t>(ty1 - ty0);
    const VkDeviceSize colorBytes = tw * th * 4 * sizeof(float);
    const VkDeviceSize depthBytes = tw * th * sizeof(std::uint32_t);
    // Two caps: kMaxTileBytes is OUR policy (an absurd zoom is the CPU lane's problem) and
    // fitsStorageBufferRange is the DEVICE's -- Vulkan 1.0 guarantees only 128 MiB, half our
    // policy cap, and colour/depth are separate bindings so each is checked (S60-alpha).
    if (colorBytes + depthBytes > kMaxTileBytes ||
        !im.ctx->caps().fitsStorageBufferRange(colorBytes) ||
        !im.ctx->caps().fitsStorageBufferRange(depthBytes))
        return false;

    // --- Mesh buffers (cached by content hash) --------------------------------------------------
    const std::uint32_t triCount = static_cast<std::uint32_t>(mesh.indices.size() / 3);
    const std::uint64_t hash = meshHash(mesh);
    // Material slots: one per distinct run in range order; per-triangle slot ids ride the cache.
    std::vector<std::size_t> slotRuns;
    constexpr VkBufferUsageFlags kSsbo =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (hash != im.cachedHash || im.cachedTris != triCount || im.verts.buf == VK_NULL_HANDLE) {
        const VkDeviceSize vb = mesh.vertices.size() * 12 * sizeof(float);
        const VkDeviceSize ib = mesh.indices.size() * sizeof(std::uint32_t);
        const VkDeviceSize tb = std::max<std::size_t>(triCount, 1) * sizeof(std::uint32_t);
        if (!im.ensureBuffer(im.verts, vb, kSsbo, Impl::Kind::Upload) ||
            !im.ensureBuffer(im.indices, ib, kSsbo, Impl::Kind::Upload) ||
            !im.ensureBuffer(im.triMat, tb, kSsbo, Impl::Kind::Upload))
            return false;
        auto* vout = static_cast<float*>(im.verts.mapped);
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const auto& v = mesh.vertices[i];
            const float row[12] = {static_cast<float>(v.position.x),
                                   static_cast<float>(v.position.y),
                                   static_cast<float>(v.position.z),
                                   v.cap,  // 1 = cap surface (reflectSidesOnly reads it)
                                   static_cast<float>(v.normal.x),
                                   static_cast<float>(v.normal.y),
                                   static_cast<float>(v.normal.z),
                                   static_cast<float>(v.uv.x),    // §12 design-space UV -- the
                                   static_cast<float>(v.uv.y),    // cap overlay map's domain
                                   static_cast<float>(v.side.x),  // unrolled side coords -- the
                                   static_cast<float>(v.side.y),  // wall map's domain (wrap mode)
                                   0.0f};
            std::memcpy(vout + i * 12, row, sizeof(row));
        }
        std::memcpy(im.indices.mapped, mesh.indices.data(), ib);
        im.cachedHash = hash;
        im.cachedTris = triCount;
    }
    // Slot mapping is cheap and deterministic; rebuild it every render (it must match triMat,
    // which was written under the same range walk when the mesh was uploaded -- rewrite both
    // whenever the ranges say so; ranges are part of the hash, so this stays consistent).
    {
        std::map<std::size_t, std::uint32_t> slotOf;
        auto* tout = static_cast<std::uint32_t*>(im.triMat.mapped);
        for (const auto& range : mesh.ranges) {
            const auto [it, inserted] =
                slotOf.emplace(range.runIndex, static_cast<std::uint32_t>(slotRuns.size()));
            if (inserted) slotRuns.push_back(range.runIndex);
            const std::uint32_t slot = it->second;
            for (std::uint32_t k = range.firstIndex / 3;
                 k < (range.firstIndex + range.indexCount) / 3; ++k)
                tout[k] = slot;
        }
    }

    // --- Per-render buffers ---------------------------------------------------------------------
    const std::size_t slotCount = std::max<std::size_t>(slotRuns.size(), 1);
    if (!im.ensureBuffer(im.mats, slotCount * 12 * sizeof(float), kSsbo, Impl::Kind::Upload) ||
        !im.ensureBuffer(im.paramsBuf, sizeof(GpuParams), kSsbo, Impl::Kind::Upload) ||
        !im.ensureBuffer(im.depth, depthBytes, kSsbo, Impl::Kind::DeviceLocal) ||
        !im.ensureBuffer(im.color, colorBytes, kSsbo | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         Impl::Kind::DeviceLocal) ||
        !im.ensureBuffer(im.staging, colorBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         Impl::Kind::Readback))
        return false;
    // The S30-e overlay maps (binding 8): every map -- the per-material design maps AND, in wrap
    // mode, their unrolled wall twins -- concatenated into one texel buffer, each material slot
    // carrying its maps' offsets/extents in its mats row (a width of 0 = no map). Uploaded per
    // render, uncached -- a render only happens on a text-cache refresh, never per composited
    // frame. A 1-texel dummy rides when overlays are off so the descriptor always has a buffer to
    // bind (the envBuf convention).
    const std::size_t mapCount = overlay != nullptr ? overlay->maps.size() : 0;
    const std::size_t wallCount = overlay != nullptr ? overlay->wallMaps.size() : 0;
    std::vector<std::uint64_t> mapTexelOffset(mapCount + wallCount, 0);
    {
        std::uint64_t total = 0;
        if (overlay != nullptr) {
            const auto place = [&](const common::ImageF& m, std::size_t at) {
                mapTexelOffset[at] = total;
                total += static_cast<std::uint64_t>(m.width) * m.height;
            };
            for (std::size_t i = 0; i < mapCount; ++i) place(overlay->maps[i], i);
            for (std::size_t i = 0; i < wallCount; ++i) place(overlay->wallMaps[i], mapCount + i);
            if (total == 0 || total > kMaxOverlayTexels) return false;  // CPU lane serves
        }
        const VkDeviceSize ovBytes =
            (overlay != nullptr ? total : 1) * 4 * sizeof(float);
        if (!im.ensureBuffer(im.overlayBuf, ovBytes, kSsbo, Impl::Kind::Upload)) return false;
        if (overlay != nullptr) {
            auto* oout = static_cast<float*>(im.overlayBuf.mapped);
            const auto copy = [&](const common::ImageF& m, std::size_t at) {
                std::memcpy(oout + mapTexelOffset[at] * 4, m.rgba.data(),
                            m.rgba.size() * sizeof(float));
            };
            for (std::size_t i = 0; i < mapCount; ++i) copy(overlay->maps[i], i);
            for (std::size_t i = 0; i < wallCount; ++i) copy(overlay->wallMaps[i], mapCount + i);
        }
    }
    {
        auto* mout = static_cast<float*>(im.mats.mapped);
        for (std::size_t s = 0; s < slotRuns.size(); ++s) {
            const Material& m = materialForRun(params, slotRuns[s]);
            float mapOff = 0.0f, mapW = 0.0f, mapH = 0.0f;
            float wallOff = 0.0f, wallW = 0.0f, wallH = 0.0f;
            if (overlay != nullptr) {
                if (const auto it = overlay->runToMap.find(slotRuns[s]);
                    it != overlay->runToMap.end()) {
                    const auto& map = overlay->maps[it->second];
                    mapOff = static_cast<float>(mapTexelOffset[it->second]);
                    mapW = static_cast<float>(map.width);
                    mapH = static_cast<float>(map.height);
                    if (it->second < wallCount) {
                        const auto& wall = overlay->wallMaps[it->second];
                        wallOff = static_cast<float>(mapTexelOffset[mapCount + it->second]);
                        wallW = static_cast<float>(wall.width);
                        wallH = static_cast<float>(wall.height);
                    }
                }
            }
            const float row[12] = {m.albedo.r,  m.albedo.g,  m.albedo.b, m.albedo.a,
                                   m.metalness, m.roughness, mapOff,     mapW,
                                   mapH,        wallOff,     wallW,      wallH};
            std::memcpy(mout + s * 12, row, sizeof(row));
        }
    }
    // The reflection snapshot (binding 7): uploaded when the app's env changes (pointer identity
    // -- a rebuilt snapshot reallocates); a tiny never-read placeholder rides when reflections
    // are off so the descriptor always has a buffer to bind.
    const bool reflectActive = params.reflectCanvas && env != nullptr && env->image != nullptr &&
                               env->image->width > 0 && env->image->height > 0 &&
                               !env->image->rgba.empty();
    {
        const VkDeviceSize envBytes =
            reflectActive ? env->image->rgba.size() * sizeof(float) : 4 * sizeof(float);
        if (!im.ensureBuffer(im.envBuf, envBytes, kSsbo, Impl::Kind::Upload)) return false;
        if (reflectActive && (im.cachedEnvData != env->image->rgba.data() ||
                              im.cachedEnvBytes != env->image->rgba.size())) {
            std::memcpy(im.envBuf.mapped, env->image->rgba.data(), envBytes);
            im.cachedEnvData = env->image->rgba.data();
            im.cachedEnvBytes = env->image->rgba.size();
        }
    }
    {
        GpuParams gp{};
        const common::Mat4 rot = params.orientation.toMat4();
        for (int c = 0; c < 4; ++c)  // column-major for GLSL
            for (int r = 0; r < 4; ++r) gp.rot[c * 4 + r] = static_cast<float>(rot.m[r][c]);
        gp.camCenterDist[0] = static_cast<float>(cam.center.x);
        gp.camCenterDist[1] = static_cast<float>(cam.center.y);
        gp.camCenterDist[2] = static_cast<float>(cam.center.z);
        gp.camCenterDist[3] = static_cast<float>(cam.camDist);
        gp.pixelLin[0] = static_cast<float>(toPixel.m00);
        gp.pixelLin[1] = static_cast<float>(toPixel.m01);
        gp.pixelLin[2] = static_cast<float>(toPixel.m10);
        gp.pixelLin[3] = static_cast<float>(toPixel.m11);
        gp.pixelOff[0] = static_cast<float>(toPixel.m02);
        gp.pixelOff[1] = static_cast<float>(toPixel.m12);
        gp.pixelOff[2] = static_cast<float>(S);
        gp.pixelOff[3] = cam.ortho ? 1.0f : 0.0f;
        gp.tile[0] = static_cast<std::int32_t>(tx0);
        gp.tile[1] = static_cast<std::int32_t>(ty0);
        gp.tile[2] = static_cast<std::int32_t>(tw);
        gp.tile[3] = static_cast<std::int32_t>(th);
        gp.ambient[0] = params.ambient.r;
        gp.ambient[1] = params.ambient.g;
        gp.ambient[2] = params.ambient.b;
        gp.ambient[3] = params.lightingEnabled ? 1.0f : 0.0f;
        const int lightCount = static_cast<int>(std::min<std::size_t>(params.lights.size(), 8));
        gp.counts[0] = static_cast<std::int32_t>(triCount);
        gp.counts[1] = lightCount;
        for (int i = 0; i < lightCount; ++i) {
            const auto& l = params.lights[static_cast<std::size_t>(i)];
            gp.lightDir[i][0] = static_cast<float>(l.direction.x);
            gp.lightDir[i][1] = static_cast<float>(l.direction.y);
            gp.lightDir[i][2] = static_cast<float>(l.direction.z);
            gp.lightDir[i][3] = l.intensity;
            gp.lightCol[i][0] = l.color.r;
            gp.lightCol[i][1] = l.color.g;
            gp.lightCol[i][2] = l.color.b;
            gp.lightCol[i][3] = 1.0f;
        }
        gp.envMisc[0] = -0.5f * std::max(0.01f, params.depth);  // the back-cap canvas plane
        gp.envMisc[1] = reflectActive ? 1.0f : 0.0f;
        gp.envMisc[2] = params.reflectSidesOnly ? 1.0f : 0.0f;  // caps keep the studio
        gp.envMisc[3] =  // §12 wrap mode: walls/bevels sample the overlay maps too
            overlay != nullptr && overlay->wrapSides ? 1.0f : 0.0f;
        if (reflectActive) {
            const common::Affine2D& le = env->layerToEnv;
            gp.envLin[0] = static_cast<float>(le.m00);
            gp.envLin[1] = static_cast<float>(le.m01);
            gp.envLin[2] = static_cast<float>(le.m10);
            gp.envLin[3] = static_cast<float>(le.m11);
            gp.envOff[0] = static_cast<float>(le.m02);
            gp.envOff[1] = static_cast<float>(le.m12);
            gp.envOff[2] = static_cast<float>(env->image->width);
            gp.envOff[3] = static_cast<float>(env->image->height);
        }
        std::memcpy(im.paramsBuf.mapped, &gp, sizeof(gp));
    }

    // --- Bind, record, dispatch twice, fence ----------------------------------------------------
    const VkBuffer bufs[Impl::kStorageBufferBindings] = {
        im.verts.buf,     im.indices.buf, im.triMat.buf,
        im.mats.buf,      im.depth.buf,   im.color.buf,
        im.paramsBuf.buf, im.envBuf.buf,  im.overlayBuf.buf};
    VkDescriptorBufferInfo infos[Impl::kStorageBufferBindings];
    VkWriteDescriptorSet writes[Impl::kStorageBufferBindings];
    for (std::uint32_t i = 0; i < Impl::kStorageBufferBindings; ++i) {
        infos[i] = {bufs[i], 0, VK_WHOLE_SIZE};
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = im.descSet,
                     .dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     .pBufferInfo = &infos[i]};
    }
    // Bindings in the shader: 0 verts, 1 indices, 2 triMat, 3 mats, 4 depth, 5 color, 6 params,
    // 7 the reflection snapshot, 8 the S30-e overlay maps.
    vkUpdateDescriptorSets(dev, Impl::kStorageBufferBindings, writes, 0, nullptr);

    vkResetCommandBuffer(im.cmd, 0);
    const VkCommandBufferBeginInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkBeginCommandBuffer(im.cmd, &cbi) != VK_SUCCESS) return false;
    vkCmdFillBuffer(im.cmd, im.depth.buf, 0, depthBytes, 0u);
    vkCmdFillBuffer(im.cmd, im.color.buf, 0, colorBytes, 0u);
    // Two distinct barriers (each access mask must be valid for its source stage): the clears
    // before the depth prepass, then the prepass's writes before the shade pass reads them.
    const VkMemoryBarrier clearBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                       .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                        VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &clearBarrier, 0, nullptr, 0,
                         nullptr);
    vkCmdBindPipeline(im.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.pipeline);
    vkCmdBindDescriptorSets(im.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, im.pipeLayout, 0, 1,
                            &im.descSet, 0, nullptr);
    const VkMemoryBarrier passBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                       VK_ACCESS_SHADER_WRITE_BIT};
    const std::uint32_t groups = (triCount + 63) / 64;
    for (std::uint32_t mode = 0; mode <= 1; ++mode) {
        vkCmdPushConstants(im.cmd, im.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mode),
                           &mode);
        vkCmdDispatch(im.cmd, groups, 1, 1);
        if (mode == 0)
            vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &passBarrier, 0,
                                 nullptr, 0, nullptr);
    }
    // Copy the finished tile out of device-local memory into the host-cached staging buffer, and
    // make the transfer visible to the host before the fence signals.
    const VkMemoryBarrier shadeToCopy{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &shadeToCopy, 0, nullptr, 0,
                         nullptr);
    const VkBufferCopy copyRegion{0, 0, colorBytes};
    vkCmdCopyBuffer(im.cmd, im.color.buf, im.staging.buf, 1, &copyRegion);
    const VkMemoryBarrier copyToHost{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                     .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                     .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(im.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &copyToHost, 0, nullptr, 0, nullptr);
    if (vkEndCommandBuffer(im.cmd) != VK_SUCCESS) return false;

    const VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                          .commandBufferCount = 1,
                          .pCommandBuffers = &im.cmd};
    vkResetFences(dev, 1, &im.fence);
    if (im.ctx->submit(si, im.fence) != VK_SUCCESS) return false;
    if (vkWaitForFences(dev, 1, &im.fence, VK_TRUE, 5'000'000'000ull) != VK_SUCCESS) return false;

    // --- Readback + the shared downsample/composite (bit-comparable with the CPU lane) ---------
    core::text::compositeSupersampledTile(dst, static_cast<const float*>(im.staging.mapped), tx0,
                                          ty0, static_cast<long>(tw), static_cast<long>(th), S);
    return true;
}

}  // namespace mosaic::render
