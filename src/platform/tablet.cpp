#include "platform/tablet.hpp"

#include <chrono>

namespace mosaic::platform {

namespace {

// Smallest power of two >= n, floored at 2 (a 1-slot ring cannot distinguish full from empty
// with free-running indices) and capped before the shift can wrap to zero and spin forever --
// a capacity that big would have failed to allocate anyway.
[[nodiscard]] std::size_t ceilPow2(std::size_t n) noexcept {
    constexpr std::size_t kTop = std::size_t{1} << 31;
    std::size_t p = 2;
    while (p < n && p < kTop)
        p <<= 1;
    return p;
}

} // namespace

SampleRing::SampleRing(std::size_t capacity) : m_buf(ceilPow2(capacity)), m_mask(m_buf.size() - 1) {}

void SampleRing::push(const TabletSample& s) noexcept {
    if (size() == m_buf.size()) {
        ++m_tail; // overwrite the OLDEST -- keep the endpoint, lose interior detail (file comment)
        ++m_overwritten;
    }
    m_buf[m_head & m_mask] = s;
    ++m_head;
}

bool SampleRing::pop(TabletSample& out) noexcept {
    if (empty())
        return false;
    out = m_buf[m_tail & m_mask];
    ++m_tail;
    return true;
}

std::uint64_t ingestClockUs() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

} // namespace mosaic::platform
