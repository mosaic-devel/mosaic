#include "render/gpu_timer.hpp"

#include <utility>

#include "common/profiler.hpp"
#include "render/gpu_caps.hpp"
#include "render/vulkan_context.hpp"

namespace mosaic::render {

// ---- Pure arithmetic ---------------------------------------------------------------------------

std::uint64_t maskTimestamp(std::uint64_t raw, std::uint32_t validBits) noexcept {
    if (validBits == 0 || validBits >= 64)
        return raw;
    return raw & ((std::uint64_t{1} << validBits) - 1ull);
}

double timestampDeltaMs(std::uint64_t begin, std::uint64_t end, std::uint32_t validBits,
                        double periodNs) noexcept {
    if (!(periodNs > 0.0))
        return 0.0;
    const std::uint64_t b = maskTimestamp(begin, validBits);
    const std::uint64_t e = maskTimestamp(end, validBits);
    // Unsigned subtraction wraps at 2^64; re-masking brings it back to the counter's own 2^N
    // domain, which is what makes a wrap across the counter's turnover read correctly instead of
    // as ~584 years.
    const std::uint64_t ticks = maskTimestamp(e - b, validBits);
    return static_cast<double>(ticks) * periodNs * 1e-6;  // ns -> ms
}

// ---- Construction ------------------------------------------------------------------------------

std::unique_ptr<GpuTimer> GpuTimer::create(const VulkanContext& ctx, std::uint32_t maxScopes,
                                           std::string& error) {
    const GpuCaps& caps = ctx.caps();
    // ASK, never assume (gpu_caps.hpp). A device without timestamps is not a broken device, it is
    // a device whose profiler rows stay at submit wall-clock -- which is what the whole tree did
    // until this class existed.
    if (!caps.timestampQueries) {
        error = "device does not support timestamp queries";
        return nullptr;
    }
    if (maxScopes == 0)
        maxScopes = kDefaultMaxScopes;

    auto self = std::unique_ptr<GpuTimer>(new GpuTimer());
    self->m_device = ctx.device();
    self->m_maxScopes = maxScopes;
    self->m_validBits = caps.timestampValidBits;
    self->m_periodNs = static_cast<double>(caps.limits.timestampPeriod);

    const VkQueryPoolCreateInfo qpci{
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = maxScopes * 2,  // one pair per scope, begin then end, adjacent
    };
    if (vkCreateQueryPool(self->m_device, &qpci, nullptr, &self->m_pool) != VK_SUCCESS) {
        error = "vkCreateQueryPool failed";
        return nullptr;
    }
    self->m_scopes.reserve(maxScopes);
    self->m_samples.reserve(maxScopes);
    return self;
}

GpuTimer::~GpuTimer() {
    if (m_pool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE)
        vkDestroyQueryPool(m_device, m_pool, nullptr);
}

// ---- Recording ---------------------------------------------------------------------------------

void GpuTimer::beginSubmission(VkCommandBuffer cmd) noexcept {
    m_scopes.clear();
    m_samples.clear();
    // Latch collection here, exactly as Profiler::Scope latches it at construction: a submission
    // that started un-instrumented must not try to read queries nobody wrote.
    m_active = common::Profiler::enabled() && m_pool != VK_NULL_HANDLE && cmd != VK_NULL_HANDLE;
    if (!m_active)
        return;
    // Every query must be reset before it is written, and the reset is recorded on the command
    // buffer because vkResetQueryPool (the host-side one) is Vulkan 1.2 and this floors on 1.0.
    // Reset the WHOLE pool, not just the scopes we expect: the count is not known yet, and an
    // un-reset query left over from a previous submission is what makes a read return stale ticks.
    vkCmdResetQueryPool(cmd, m_pool, 0, m_maxScopes * 2);
}

std::int32_t GpuTimer::beginScope(VkCommandBuffer cmd, std::string_view name) noexcept {
    if (!m_active || m_scopes.size() >= m_maxScopes)
        return -1;
    const auto idx = static_cast<std::uint32_t>(m_scopes.size());
    m_scopes.push_back(ScopeRec{name, idx * 2, idx * 2 + 1, false});
    // TOP_OF_PIPE for the open, BOTTOM_OF_PIPE for the close: the pair then brackets everything
    // between them on the device timeline. (Both are legal at any point in a command buffer; the
    // stage says WHEN the counter is sampled relative to the commands around it.)
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_pool, idx * 2);
    return static_cast<std::int32_t>(idx);
}

void GpuTimer::endScope(VkCommandBuffer cmd, std::int32_t scope) noexcept {
    if (!m_active || scope < 0 || static_cast<std::size_t>(scope) >= m_scopes.size())
        return;
    ScopeRec& s = m_scopes[static_cast<std::size_t>(scope)];
    if (s.closed)
        return;  // closing twice would write a second timestamp into a query already written
    s.closed = true;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, s.endQuery);
}

std::uint32_t GpuTimer::resolveAndRecord() noexcept {
    m_samples.clear();
    if (!m_active) {
        m_scopes.clear();
        return 0;
    }
    m_active = false;

    std::uint32_t recorded = 0;
    for (const ScopeRec& s : m_scopes) {
        if (!s.closed)
            continue;  // an abandoned scope (an early return) simply has no sample
        std::uint64_t ticks[2] = {0, 0};
        // Read the pair on its own, and WITHOUT VK_QUERY_RESULT_WAIT_BIT. Two reasons, both about
        // never hanging the UI thread for a diagnostic: WAIT on a query that was reset and never
        // written does not return, and a batched read over the whole pool would report NOT_READY
        // for every scope if any one of them were open. The caller has already waited on the
        // submission's fence, so VK_SUCCESS is the ordinary outcome here and anything else means
        // "drop this sample", never "block".
        const VkResult r =
            vkGetQueryPoolResults(m_device, m_pool, s.beginQuery, 2, sizeof(ticks), ticks,
                                  sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS)
            continue;
        const double ms = timestampDeltaMs(ticks[0], ticks[1], m_validBits, m_periodNs);
        m_samples.push_back(Sample{s.name, ms});
        common::Profiler::instance().record(s.name, common::Lane::GpuDevice, ms);
        ++recorded;
    }
    m_scopes.clear();
    return recorded;
}

}  // namespace mosaic::render
