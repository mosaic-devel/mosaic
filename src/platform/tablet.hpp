#pragma once

#include "common/geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// The unified tablet event model (docs/tablet.md §2). Every backend -- XInput2 today,
// zwp_tablet_v2 next, the S57/S58 Windows/macOS ports later -- normalizes its device stream into
// `TabletSample`s; everything downstream (the policy layer in core/brush/, the stroke engine, the
// Settings->Tablet test area) consumes only this struct and never a platform event.
//
// FLTK-free and headless-testable by design: a backend is a *source of samples*, the tests feed a
// canned stream, and the FLTK hookup (Fl::add_system_handler, the canvas drain) lives with the
// consumer, never here. Derived sensors (speed, drawingangle, distance, fade, ...) are computed in
// core/brush/ from the sample stream, not by this layer.
namespace mosaic::platform {

// How the X11 backend resolved each sample's POSITION, and how its mapping guard is behaving
// (tablet_x11.hpp's xi2ParseEvent). Lives HERE, not in tablet_x11.hpp, because tablet_x11.hpp drags
// in Xlib and its macros -- and the UI layer, which reads this, must never see those.
//
// Diagnostic, but not decoration: a stroke that MIXES the two position sources is worse than one
// that uses either consistently, so `fromServer` climbing mid-stroke on a device that HAS position
// axes is a bug. That is how the fast-stroke wobble was caught.
struct TabletPositionDiag {
    std::size_t fromValuators = 0;  // the DEVICE's own coordinates -- what we want
    std::size_t fromServer = 0;     // the fallback: the server's pointer, which wobbles
    int untrusts = 0;               // times an ALREADY-TRUSTED device was kicked back out
    double worstDelta = 0.0;        // largest |valuator - server| ever seen
    double worstUntrustDelta = 0.0; // ... and the largest that actually unseated a device
};

// Tilt full-scale, in degrees. Backends whose driver reports tilt as a unitless range (XI2
// valuators) map the device's full range onto +/-this, so hardware full-tilt lands exactly on the
// sensor layer's full scale (core::brush::kMaxTiltDegrees -- the two constants are pinned to the
// same 60 by docs/tablet.md §2 + docs/brushes.md §6.2). Backends that already speak degrees
// (zwp_tablet_v2's wl_fixed degrees, NSEvent) pass their value through unscaled instead.
inline constexpr double kTiltFullScaleDegrees = 60.0;

// What the pen's cursor should look like. Platform-neutral and FLTK-free by construction (the
// platform layer never sees an Fl_Cursor); the wiring maps FLTK's cursors onto these.
//
// This exists because a client that binds the tablet manager OWNS its tool's cursor: the compositor
// stops drawing one for it, and one that never sets it gets the compositor's default -- on KWin a
// crosshair, over every pixel of the UI. FLTK cannot do it for us, because its cursor calls go
// through wl_pointer.set_cursor and a tablet tool HAS no wl_pointer. Only the shapes Mosaic actually
// asks for; anything else maps to Default.
enum class TabletCursor {
    Hidden,    // the canvas under a brush: the GPU reticle ring IS the cursor
    Default,   // the arrow
    Crosshair, // selection, crop
    Text,
    Move,
    Pointer, // the hand
    Wait,
};

struct TabletSample {
    common::Vec2 pos{};              // surface-local coords, SUB-PIXEL (the whole point, §6.2)
    double pressure = 1.0;           // [0,1], normalized by the device's reported axis maximum
    double xTilt = 0.0;              // degrees
    double yTilt = 0.0;              // degrees
    double rotation = 0.0;           // degrees; barrel/art-pen rotation, [-180,180]
    double tangentialPressure = 0.0; // [0,1]; airbrush finger wheel
    enum class Tool { Pen, Eraser, Airbrush, Puck, Mouse } tool = Tool::Pen;
    std::uint64_t toolSerial = 0;    // per-stylus unique id, 0 if unavailable
    bool inProximity = false;
    std::uint32_t buttons = 0;       // bit N = platform button N+1 held (X11 buttons 1..32)
    std::uint64_t timeUs = 0;        // OUR clock at ingest, NEVER the driver's (§5)
    // WHICH of our windows this came from, as an opaque id: the X11 Window, or the wl_surface
    // pointer. 0 = synthesized (a mouse) or unknown.
    //
    // Load-bearing, and not merely diagnostic. A backend selects tablet events on EVERY window of
    // ours that asks for them, because Settings -> Tablet's test area has to read the pen while it
    // hovers THE DIALOG -- that is the entire point of a test area, and selecting only on the main
    // window is why it used to answer nothing unless the pen was over the canvas. But `pos` is
    // SURFACE-LOCAL: a sample taken over the dialog is in the dialog's coordinates, and feeding it
    // to a canvas stroke would paint at a lie. So every sample says where it came from, and the
    // wiring feeds the stroke only from the canvas's own surface (§8).
    std::uint64_t surface = 0;
};

// The sample queue between a backend and the canvas drain (docs/tablet.md §3.1). Producer and
// consumer both run on the FLTK thread -- the X11 system handler and the Wayland dispatch both
// fire inside Fl::wait(), and the drain runs in the event handler -- so plain indices suffice:
// no atomics, no locking in the paint path.
//
// A full ring OVERWRITES THE OLDEST sample rather than dropping the newest: under a stall the
// stroke loses interior detail but keeps its endpoint current, which is the difference between a
// slightly simplified curve and a stroke that lags the pen. `overwritten()` counts what was lost,
// honestly -- diagnostics (§8's event logging) read it, and a nonzero count in tests means the
// drain cadence is wrong.
class SampleRing {
public:
    // `capacity` is rounded up to a power of two (minimum 2). The default holds ~2.5s of 200 Hz
    // samples -- far beyond any real drain gap, so overwrite only fires under a genuine stall.
    explicit SampleRing(std::size_t capacity = 512);

    void push(const TabletSample& s) noexcept;

    // Oldest-first. Returns false (and leaves `out` untouched) when empty.
    [[nodiscard]] bool pop(TabletSample& out) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return m_head - m_tail; }
    [[nodiscard]] bool empty() const noexcept { return m_head == m_tail; }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_buf.size(); }
    [[nodiscard]] std::uint64_t overwritten() const noexcept { return m_overwritten; }

    // Drops queued samples; the overwritten count survives (it is a lifetime diagnostic).
    void clear() noexcept { m_tail = m_head; }

private:
    std::vector<TabletSample> m_buf;
    std::size_t m_mask = 0;
    // Free-running indices masked on access. size() stays correct across index wraparound
    // because the difference of two unsigned free-running counters is wrap-safe.
    std::size_t m_head = 0; // next write
    std::size_t m_tail = 0; // next read
    std::uint64_t m_overwritten = 0;
};

// What every platform backend presents to the canvas wiring. Deliberately small: a backend is a
// source of samples and a statement of whether it is working. Lifecycle differences stay behind
// it -- on X11 the FLTK FL_PUSH/DRAG/RELEASE stream drives the stroke and drains ring() (§3.1);
// on Wayland the backend owns the stroke lifecycle itself (§4 built-note, finding 4) and will
// grow the callbacks it needs when it lands.
class TabletBackend {
public:
    virtual ~TabletBackend() = default;

    // False = report-and-fall-back (§3.2): the canvas synthesizes pressure-1 samples from core
    // pointer events and a stroke still paints. Never a hard failure.
    [[nodiscard]] virtual bool available() const noexcept = 0;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    [[nodiscard]] virtual SampleRing& ring() noexcept = 0;
};

// Microseconds on OUR monotonic clock, for TabletSample::timeUs. Stamped at ingest on every
// platform because driver clocks are unreliable by design (docs/tablet.md §5: some WinTab drivers
// report milliseconds since boot; X server time is a different epoch than wl timestamps). The
// epoch is unspecified -- only deltas are meaningful, which is all the stroke clock reads.
[[nodiscard]] std::uint64_t ingestClockUs() noexcept;

} // namespace mosaic::platform
