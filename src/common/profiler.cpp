#include "common/profiler.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::common {

// The one definition of the collection master switch. Debug builds default it ON (the Help-menu
// FPS readout and Timing Profiler window work with no flag, exactly as before S60-alpha); release
// defaults OFF and is opted into with --profile / MOSAIC_PROFILE=1. Deciding it HERE -- in a single
// translation unit compiled as part of mosaic_common -- is what keeps consumers that do not define
// MOSAIC_DEBUG from disagreeing with those that do.
#ifdef MOSAIC_DEBUG
std::atomic<bool> Profiler::s_enabled{true};
#else
std::atomic<bool> Profiler::s_enabled{false};
#endif

Profiler& Profiler::instance() {
    static Profiler p;
    return p;
}

void Profiler::record(std::string_view name, Lane lane, double ms) {
    const std::lock_guard<std::mutex> lk(m_mutex);
    const auto key = std::make_pair(std::string(name), lane);
    const auto now = std::chrono::steady_clock::now();
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
        Entry e;
        e.lane = lane;
        e.name = key.first;
        e.count = 1;
        e.last = e.min = e.max = e.sum = ms;
        e.lastSeen = now;
        m_entries.emplace(key, std::move(e));
    } else {
        Entry& e = it->second;
        ++e.count;
        e.last = ms;
        e.sum += ms;
        e.min = std::min(e.min, ms);
        e.max = std::max(e.max, ms);
        e.lastSeen = now;
    }
}

void Profiler::frameTick(double intervalMs) {
    const std::lock_guard<std::mutex> lk(m_mutex);
    if (intervalMs > 0.0 && intervalMs < 1000.0)
        m_avgFrameMs = m_avgFrameMs <= 0.0 ? intervalMs : 0.9 * m_avgFrameMs + 0.1 * intervalMs;
}

double Profiler::fps() const {
    const std::lock_guard<std::mutex> lk(m_mutex);
    return m_avgFrameMs > 1e-4 ? 1000.0 / m_avgFrameMs : 0.0;
}

double Profiler::frameMs() const {
    const std::lock_guard<std::mutex> lk(m_mutex);
    return m_avgFrameMs;
}

std::vector<ProfileRow> Profiler::snapshot() const {
    const std::lock_guard<std::mutex> lk(m_mutex);
    const auto now = std::chrono::steady_clock::now();
    std::vector<ProfileRow> rows;
    rows.reserve(m_entries.size());
    for (const auto& [key, e] : m_entries) {
        ProfileRow r;
        r.name = e.name;
        r.lane = e.lane;
        r.last = e.last;
        r.min = e.min;
        r.max = e.max;
        r.avg = e.count != 0 ? e.sum / static_cast<double>(e.count) : 0.0;
        r.count = e.count;
        r.ageSec = std::chrono::duration<double>(now - e.lastSeen).count();
        const double recency = std::exp(-r.ageSec / kHeatTauSec);
        const double slowness = std::clamp((r.last - kHotMs) / (kVeryHotMs - kHotMs), 0.0, 1.0);
        r.heat = recency * slowness;
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(),
              [](const ProfileRow& a, const ProfileRow& b) { return a.max > b.max; });
    return rows;
}

void Profiler::clear() {
    const std::lock_guard<std::mutex> lk(m_mutex);
    m_entries.clear();
    m_avgFrameMs = 0.0;
}

} // namespace mosaic::common
