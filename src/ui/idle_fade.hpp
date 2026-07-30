#pragma once

namespace mosaic::ui {

// The empty-state idle pass's fade choreography: pure time arithmetic (no FLTK, no Vulkan), so
// the timings below -- the design round's port targets (2026-07-23) -- are unit-testable. The
// canvas evaluates this once per frame and pushes the result to the renderer.
//
// The design rule the shapes encode: the field SETTLES rather than dissolves. On a document's
// arrival amplitude ramps down with alpha (cubic-in, quick); on the return to blank there is a
// beat of stillness, then the field blooms back cubic-out with its wave phase still running --
// the clock never resets, so the canvas resumes breathing instead of restarting.

// One eased scalar transition; evaluate with value(now). start() rebases on the CURRENT value,
// so retargeting mid-flight never jumps.
struct IdleTween {
    double from = 0.0;
    double to = 0.0;
    double t0 = -1.0e18; // start time (already in the past for the initial state)
    double dur = 0.0;
    bool easeIn = false; // cubic-in (the settle) vs cubic-out (the bloom and the answers)

    void start(double now, double target, double duration, bool in, double delay = 0.0) {
        from = value(now);
        to = target;
        t0 = now + delay;
        dur = duration;
        easeIn = in;
    }
    [[nodiscard]] double value(double now) const {
        if (now <= t0)
            return from;
        if (dur <= 0.0 || now >= t0 + dur)
            return to;
        const double x = (now - t0) / dur;
        const double inv = 1.0 - x;
        return from + (to - from) * (easeIn ? x * x * x : 1.0 - inv * inv * inv);
    }
};

// The whole choreography: the field's presence, the invitation quad's own opacity, the
// drag-over bloom and the pointer-hover row crossfade.
struct IdleFadeState {
    // Timings in seconds.
    static constexpr double kFieldOutDur = 0.18; // field settles while the document lands
    static constexpr double kFieldInDelay = 0.08; // the beat of stillness on return
    static constexpr double kFieldInDur = 0.42;   // ... then the bloom
    static constexpr double kInvOutDur = 0.15;
    static constexpr double kInvInDelay = 0.16;
    static constexpr double kInvInDur = 0.34;
    static constexpr double kHotInDur = 0.16;
    static constexpr double kHotOutDur = 0.24;
    static constexpr double kHoverDur = 0.12;

    IdleTween field;
    IdleTween invitation;
    IdleTween hot;
    IdleTween hover;
    bool enabled = false;

    void setEnabled(bool on, double now) {
        if (on == enabled)
            return;
        enabled = on;
        if (on) {
            field.start(now, 1.0, kFieldInDur, /*in=*/false, kFieldInDelay);
            invitation.start(now, 1.0, kInvInDur, /*in=*/false, kInvInDelay);
        } else {
            field.start(now, 0.0, kFieldOutDur, /*in=*/true);
            invitation.start(now, 0.0, kInvOutDur, /*in=*/true);
            hot.start(now, 0.0, kHotOutDur, /*in=*/false);
            hover.start(now, 0.0, kHoverDur, /*in=*/false);
        }
    }
    void setHot(bool on, double now) {
        const double target = on ? 1.0 : 0.0;
        if (hot.to == target)
            return; // FL_DND_DRAG fires per pointer motion: restarting would stretch the bloom
        hot.start(now, target, on ? kHotInDur : kHotOutDur, /*in=*/false);
    }
    void setHover(bool on, double now) {
        const double target = on ? 1.0 : 0.0;
        if (hover.to == target)
            return;
        hover.start(now, target, kHoverDur, /*in=*/false);
    }

    // Any visible contribution: while enabled, or while a fade-out is still in flight (the
    // renderer skips the pass entirely once this goes false).
    [[nodiscard]] bool active(double now) const {
        return enabled || field.value(now) > 0.0 || invitation.value(now) > 0.0;
    }
};

} // namespace mosaic::ui
