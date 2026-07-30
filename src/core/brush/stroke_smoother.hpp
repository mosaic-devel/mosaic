#pragma once

#include "core/brush/stroke_state.hpp" // StrokeInput

#include <cstddef>
#include <vector>

// Brush smoothing (docs/tablet.md §7).
//
// WHY THIS EXISTS, AND WHY THE SPLINE WAS NOT ENOUGH. `stroke_path.hpp` lays dabs along a curve
// THROUGH the samples. That fixed the polygon -- a 60 Hz mouse stroke was literally a 60-gon -- and it
// could never have fixed the rest, because a curve through the samples reproduces the samples exactly,
// and a mouse's samples are INTEGER positions at 60 Hz (the canvas synthesizes them from
// Fl::event_x(), which is an int). The jaggedness that remains is NOISE IN THE INPUT, and
// interpolation is *defined* to preserve it. To remove it, something has to MOVE the points.
//
// So this is a FILTER, deliberately, and it is the one place in the brush pipeline that is allowed to
// be. The form is the classical weighted moving average -- the lineage is Savitzky-Golay (1964) --
// applied here to real-time freehand pointer input.
//
// ⚠⚠⚠ THE ONE INVARIANT OF THIS FILE: **THE WINDOW IS FIXED. IT MUST NEVER ADAPT TO SPEED.**
//
// "Shrink the window when the pointer moves fast, so it feels more responsive" is EXACTLY the obvious
// improvement a later session would reach for. Do not. Not behind a flag, not as an option, not "just
// for the tablet". The window is a function of the user's strength setting alone, and the weights
// below are a function of SAMPLE INDEX and nothing else -- they do not know how fast the pointer is
// moving, and they must not learn. That costs responsiveness on a fast stroke, and it is a hard
// constraint on this file, deliberately paid, not an oversight -- do not "improve" it away.
//
// This is NOT the rope / pulled-string stabilizer (docs/tablet.md §7), which drags a virtual anchor
// behind the cursor on a leash. That is a separate, deferred mechanism; nothing here implements it.
namespace mosaic::core::brush {

// How hard to smooth. `strength` is the user-facing knob in [0,1]; 0 is OFF and is an exact identity
// (the sample comes out byte-for-byte as it went in), which is what keeps every existing golden and
// every tablet stroke unchanged unless the user asks otherwise.
struct SmoothingParams {
    double strength = 0.0;

    [[nodiscard]] bool active() const noexcept { return strength > 0.0; }
};

// The largest window the smoother will ever average over. A BOUND on a FIXED window -- not a target,
// and never a function of speed.
inline constexpr std::size_t kMaxSmoothingWindow = 24;

// Smoothing is a BINARY choice to the user -- a toggle, not a slider (user call, 2026-07-11). There
// is no useful "a little bit of rattle": either the pointer's noise is filtered or it is not, and the
// in-between settings were a dial with nothing on it. The filter still takes a strength, because the
// maths does; this is the one the toggle means.
//
// ⚠ The window is measured in SAMPLES, so what this costs in TIME depends on how fast the device
// reports -- and that is the number to look at before touching it. Measured:
//
//        strength 1  ->  200 Hz pen: 22 ms lag  |  144 Hz mouse: 31 ms  |  60 Hz mouse: 75 ms
//
// 75 ms is a stroke visibly trailing the pointer, which is the thing a stabilizer is disliked for.
// Counting the window in SAMPLES rather than in speed is what gives the noisy 60 Hz mouse more
// averaging than the clean 200 Hz pen -- but the lag rides along with it. Do not raise this without
// measuring on a 60 Hz display.
inline constexpr double kSmoothingOnStrength = 1.0;

// The number of samples averaged at a given strength. Pure, and pinned by a test: `strength` is the
// ONLY input. If this function ever grows a speed, a velocity, a distance or a time argument, the
// fixed-window invariant above has been broken.
[[nodiscard]] std::size_t smoothingWindow(double strength) noexcept;

// A Gaussian weighted average over the last N input positions.
//
// POSITION ONLY. Pressure, tilt, rotation and the timestamp pass through from the newest sample,
// untouched: smoothing the pen's *path* is what the user asked for, and averaging its pressure would
// mush the very dynamics the tablet work exists to deliver.
class StrokeSmoother {
public:
    void setParams(const SmoothingParams& p) noexcept { m_params = p; }
    [[nodiscard]] const SmoothingParams& params() const noexcept { return m_params; }

    // Start a stroke. The first sample is never smoothed -- there is nothing to smooth it against, and
    // a stroke must begin exactly where the user pressed.
    void begin(const StrokeInput& first);

    // Feed one raw sample; get back the one to paint with. With smoothing off this returns `raw`
    // unchanged, bit for bit.
    [[nodiscard]] StrokeInput smooth(const StrokeInput& raw);

    // The stroke is ending. Returns the samples still owed, in order, so the stroke ENDS WHERE THE
    // POINTER ENDED rather than short of it: an averaged point necessarily trails the raw input, so a
    // stroke that simply stopped would fall short of the last thing the user did. The tail ramps the
    // window down to nothing, and the final sample is the user's own, unsmoothed.
    //
    // Empty when smoothing is off (there is nothing owed) or no stroke is running.
    [[nodiscard]] std::vector<StrokeInput> flush();

private:
    SmoothingParams m_params;
    std::vector<StrokeInput> m_history; // newest last; never longer than kMaxSmoothingWindow
    bool m_active = false;
};

} // namespace mosaic::core::brush
