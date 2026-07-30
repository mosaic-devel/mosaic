#include "core/brush/stroke_smoother.hpp"

#include <algorithm>
#include <cmath>

namespace mosaic::core::brush {

namespace {

// The Gaussian's width, in SAMPLES, at full strength. Paired with kMaxSmoothingWindow so the tail of
// the kernel is negligible at the window's edge rather than chopped off mid-slope.
constexpr double kSigmaAtFullStrength = 6.0;

[[nodiscard]] double clamp01(double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

} // namespace

std::size_t smoothingWindow(double strength) noexcept {
    // ⚠⚠ `strength` is the ONLY argument, and that is the point (stroke_smoother.hpp). The window is
    // a function of what the USER asked for and nothing else -- not of speed, not of velocity, not of
    // the distance between samples, not of the time between them. A fixed window is a hard invariant
    // here: never give this function a speed, and never make the average adapt to one.
    const double s = clamp01(strength);
    if (s <= 0.0)
        return 1; // off: the average of one sample is that sample
    const auto n = static_cast<std::size_t>(std::lround(1.0 + s * (kMaxSmoothingWindow - 1)));
    return std::clamp<std::size_t>(n, 1, kMaxSmoothingWindow);
}

void StrokeSmoother::begin(const StrokeInput& first) {
    m_history.clear();
    m_history.push_back(first);
    m_active = true;
}

StrokeInput StrokeSmoother::smooth(const StrokeInput& raw) {
    if (!m_active)
        return raw;
    if (!m_params.active())
        return raw; // OFF is an exact identity -- the sample comes out as it went in, bit for bit

    m_history.push_back(raw);
    const std::size_t window = smoothingWindow(m_params.strength);
    if (m_history.size() > window)
        m_history.erase(m_history.begin(), m_history.end() - static_cast<std::ptrdiff_t>(window));

    // Gaussian weights over the window, by SAMPLE INDEX. The newest sample sits at the peak, so the
    // stroke still follows the pointer; the older ones pull it toward where the pointer has actually
    // been, which is what removes the tremor and the pixel-grid quantisation.
    const double sigma = std::max(1e-6, kSigmaAtFullStrength * clamp01(m_params.strength));
    double wsum = 0.0;
    common::Vec2 acc{0.0, 0.0};
    const auto n = static_cast<double>(m_history.size());
    for (std::size_t i = 0; i < m_history.size(); ++i) {
        const double age = n - 1.0 - static_cast<double>(i); // 0 = the newest sample
        const double w = std::exp(-(age * age) / (2.0 * sigma * sigma));
        acc = acc + m_history[i].pos * w;
        wsum += w;
    }

    // POSITION ONLY. Everything else is the newest sample's, untouched: averaging pressure would mush
    // the very dynamics the tablet work exists to deliver, and averaging the timestamp would lie about
    // when the sample happened.
    StrokeInput out = raw;
    if (wsum > 0.0)
        out.pos = acc * (1.0 / wsum);
    return out;
}

std::vector<StrokeInput> StrokeSmoother::flush() {
    std::vector<StrokeInput> owed;
    if (!m_active)
        return owed;
    m_active = false;
    if (!m_params.active() || m_history.size() < 2) {
        m_history.clear();
        return owed; // nothing was ever held back
    }

    // An averaged point necessarily TRAILS the raw input -- that is what a filter does. So a stroke
    // that simply stopped would fall SHORT of the last thing the user did: the pen lifts at the end of
    // a flick and the paint never gets there. Ramp the window down to nothing, re-smoothing the tail
    // against an ever-shorter history, and finish on the user's own final sample, unsmoothed. The
    // stroke then ends exactly where the pointer ended.
    const StrokeInput last = m_history.back();
    std::vector<StrokeInput> hist = m_history;
    // Stop the ramp ONE step short of the end. Run it all the way and its final step reduces to a
    // window of one -- which is the raw last sample -- so the explicit push below would emit that
    // point TWICE. (The engine absorbs a zero-travel sample, so it was harmless; it was also a lie
    // about what the ramp does, and a mutation test caught it by removing the push and changing
    // nothing at all.)
    for (std::size_t drop = 1; drop + 1 < hist.size(); ++drop) {
        const std::size_t remaining = hist.size() - drop;
        const double sigma = std::max(1e-6, kSigmaAtFullStrength * clamp01(m_params.strength) *
                                                (static_cast<double>(remaining) /
                                                 static_cast<double>(hist.size())));
        double wsum = 0.0;
        common::Vec2 acc{0.0, 0.0};
        for (std::size_t i = drop; i < hist.size(); ++i) {
            const double age = static_cast<double>(hist.size() - 1 - i);
            const double w = std::exp(-(age * age) / (2.0 * sigma * sigma));
            acc = acc + hist[i].pos * w;
            wsum += w;
        }
        StrokeInput s = last;
        if (wsum > 0.0)
            s.pos = acc * (1.0 / wsum);
        owed.push_back(s);
    }
    owed.push_back(last); // ... and the user's own last point, exactly where they left it
    m_history.clear();
    return owed;
}

} // namespace mosaic::core::brush
